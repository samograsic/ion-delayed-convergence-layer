/*
	udpmoondelayclo.c:	UDP Moon Delay convergence-layer output daemon
				with simplified single-threaded queue processing and link loss simulation.

	Based on original ION UDP convergence layer (udpclo.c)
	Author: Samo Grasic (samo@grasic.net), LateLab AB, Sweden

	Copyright (c) 2006, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship acknowledged.
*/

#include "udpcla.h"
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

/* Moon delay constants */
#define SPEED_OF_LIGHT 299792.458      /* km/s */
#define MOON_DISTANCE_AVG 384400.0     /* km, average Earth-Moon distance */
#define MOON_DISTANCE_VAR 20000.0      /* km, distance variation */
#define MOON_ORBITAL_PERIOD 27.3       /* days */

/* Link loss simulation - can be modified at compile time */
#ifndef LINK_LOSS_PERCENTAGE
#define LINK_LOSS_PERCENTAGE 10.0  /* 0.0 = no loss, 5.0 = 5% loss */
#endif

/* Simple bundle queue management - single threaded */
#define MAX_QUEUED_BUNDLES 100

typedef struct {
	Object bundleZco;
	BpAncillaryData ancillaryData;
	unsigned int bundleLength;
	struct timeval sendTime;     /* When to send this bundle */
} QueuedBundle;

typedef struct {
	QueuedBundle bundles[MAX_QUEUED_BUNDLES];
	int count;
} BundleQueue;

static BundleQueue queue;
static int g_running = 1;
static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t monitorThread;
static int ductSocket;
static struct sockaddr socketName;
static unsigned char *globalBuffer;

static sm_SemId		udpmoondelaycloSemaphore(sm_SemId *semid)
{
	static sm_SemId	semaphore = -1;
	
	if (semid)
	{
		semaphore = *semid;
	}

	return semaphore;
}

/* Simulate link loss - returns 1 if bundle should be dropped */
static int shouldDropBundle(void)
{
	if (LINK_LOSS_PERCENTAGE <= 0.0) {
		return 0;  /* No loss */
	}
	
	/* Generate random number between 0.0 and 100.0 */
	double random = ((double)rand() / RAND_MAX) * 100.0;
	return (random < LINK_LOSS_PERCENTAGE) ? 1 : 0;
}

/* Forward declaration */
static void processReadyBundles(int socket, struct sockaddr *sockName, unsigned char *buffer);

/* Calculate Moon delay based on current lunar position */
static double calculateMoonDelay(void)
{
	time_t now = time(NULL);
	double moonPhase, distance;
	
	/* Calculate Moon's position in its orbit */
	moonPhase = fmod((double)(now / 86400.0) * 2.0 * M_PI / MOON_ORBITAL_PERIOD, 2.0 * M_PI);
	
	/* Calculate distance using sinusoidal variation */
	distance = MOON_DISTANCE_AVG + (MOON_DISTANCE_VAR * cos(moonPhase));
	
	/* Convert to light-travel time */
	return distance / SPEED_OF_LIGHT;
}

/* Initialize bundle queue */
static void initQueue(void)
{
	memset(&queue, 0, sizeof(BundleQueue));
}

/* Add bundle to queue */
static int addBundle(Object bundleZco, BpAncillaryData *ancillaryData, unsigned int bundleLength)
{
	pthread_mutex_lock(&queueMutex);
	if (queue.count >= MAX_QUEUED_BUNDLES) {
		pthread_mutex_unlock(&queueMutex);
		return -1;  /* Queue full */
	}
	
	QueuedBundle *bundle = &queue.bundles[queue.count];
	bundle->bundleZco = bundleZco;
	bundle->ancillaryData = *ancillaryData;
	bundle->bundleLength = bundleLength;
	
	/* Calculate send time = current time + delay */
	struct timeval now;
	gettimeofday(&now, NULL);
	bundle->sendTime = now;
	double delaySeconds = calculateMoonDelay();
	long long delayMicroseconds = (long long)(delaySeconds * 1000000.0);
	bundle->sendTime.tv_usec += delayMicroseconds;
	
	/* Handle overflow */
	while (bundle->sendTime.tv_usec >= 1000000) {
		bundle->sendTime.tv_sec++;
		bundle->sendTime.tv_usec -= 1000000;
	}
	
	queue.count++;
	
	/* Debug: Log bundle queuing */
	{
		char debugMsg[128];
		snprintf(debugMsg, sizeof(debugMsg), "[DEBUG] udpmoondelayclo: Queued bundle (queue size: %d, delay: %.1f sec)", 
				queue.count, delaySeconds);
		writeMemo(debugMsg);
	}
	
	pthread_mutex_unlock(&queueMutex);
	return 0;
}

/* Send a bundle (after delay has elapsed) */
static int sendBundle(int ductSocket, struct sockaddr *socketName, 
		      QueuedBundle *bundle, unsigned char *buffer)
{
	/* Check for link loss */
	if (shouldDropBundle()) {
		/* Simulate bundle loss - just drop it and release ZCO */
		zco_destroy(getIonsdr(), bundle->bundleZco);
		return 0;
	}
	
	/* Extract bundle content from ZCO */
	Sdr sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));
	ZcoReader reader;
	zco_start_transmitting(bundle->bundleZco, &reader);
	int bytesToSend = zco_transmit(sdr, &reader, bundle->bundleLength, (char *)buffer);
	if (bytesToSend != bundle->bundleLength) {
		sdr_exit_xn(sdr);
		putErrmsg("Can't read bundle content.", NULL);
		return -1;
	}
	sdr_exit_xn(sdr);
	
	/* Send the bundle via UDP */
	int bytesSent = isendto(ductSocket, (char *)buffer, bytesToSend, 0, socketName, sizeof(struct sockaddr_in));
	if (bytesSent < 0) {
		putSysErrmsg("Can't send bundle.", NULL);
		return -1;
	}
	
	/* Debug: Log successful transmission */
	{
		char debugMsg[128];
		snprintf(debugMsg, sizeof(debugMsg), "[DEBUG] udpmoondelayclo: Sent bundle (%d bytes)", bytesSent);
		writeMemo(debugMsg);
	}
	
	/* Clean up ZCO */
	CHKZERO(sdr_begin_xn(sdr));
	zco_destroy(sdr, bundle->bundleZco);
	if (sdr_end_xn(sdr) < 0) {
		putErrmsg("Can't destroy bundle ZCO.", NULL);
		return -1;
	}
	
	return 0;
}

/* Monitor thread function - continuously checks and sends ready bundles */
static void* queueMonitorThread(void* arg)
{
	writeMemo("[DEBUG] udpmoondelayclo: Monitor thread started");
	
	while (g_running) {
		processReadyBundles(ductSocket, &socketName, globalBuffer);
		
		/* Sleep for 10ms to avoid busy waiting but maintain responsiveness */
		microsnooze(10000);
	}
	
	writeMemo("[DEBUG] udpmoondelayclo: Monitor thread ending");
	return NULL;
}

/* Process ready bundles and wait for exact timing */
static void processReadyBundles(int socket, struct sockaddr *sockName, unsigned char *buffer)
{
	struct timeval now;
	int processed = 0;
	
	pthread_mutex_lock(&queueMutex);
	
	for (int i = 0; i < queue.count; i++) {
		QueuedBundle *bundle = &queue.bundles[i];
		
		/* Check if this bundle is ready to be sent */
		gettimeofday(&now, NULL);
		
		
		if (now.tv_sec > bundle->sendTime.tv_sec ||
		    (now.tv_sec == bundle->sendTime.tv_sec && 
		     now.tv_usec >= bundle->sendTime.tv_usec)) {
			
			
			/* Send the bundle */
			if (sendBundle(socket, sockName, bundle, buffer) < 0) {
				putErrmsg("Can't send bundle.", NULL);
			}
			
			/* Mark for removal */
			bundle->bundleLength = 0;
			processed++;
		}
	}
	
	/* Remove processed bundles by compacting array */
	if (processed > 0) {
		int writeIndex = 0;
		for (int readIndex = 0; readIndex < queue.count; readIndex++) {
			if (queue.bundles[readIndex].bundleLength > 0) {
				if (writeIndex != readIndex) {
					queue.bundles[writeIndex] = queue.bundles[readIndex];
				}
				writeIndex++;
			}
		}
		queue.count = writeIndex;
	}
	
	pthread_mutex_unlock(&queueMutex);
}

/* Cleanup queue */
static void destroyQueue(void)
{
	Sdr sdr = getIonsdr();
	
	/* Clean up any remaining ZCOs */
	if (sdr_begin_xn(sdr) >= 0) {
		for (int i = 0; i < queue.count; i++) {
			if (queue.bundles[i].bundleZco != 0) {
				zco_destroy(sdr, queue.bundles[i].bundleZco);
			}
		}
		sdr_exit_xn(sdr);
	}
	queue.count = 0;
}

static void shutDownClo(int signum)
{
	isignal(SIGTERM, shutDownClo);
	isignal(SIGINT, shutDownClo);
	isignal(SIGHUP, shutDownClo);
	writeMemo("[i] udpmoondelayclo received shutdown signal, terminating gracefully...");
	g_running = 0;
	sm_SemEnd(udpmoondelaycloSemaphore(NULL));
}

#if defined (ION_LWT)
int	udpmoondelayclo(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char	*ductName = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*ductName = (argc > 1 ? argv[1] : NULL);
#endif
	VOutduct		*vduct;
	PsmAddress		vductElt;
	Sdr			sdr;
	Outduct			outduct;
	Object			planDuctList;
	Object			planObj = 0;
	BpPlan			plan;
	IonNeighbor		*neighbor = NULL;
	PsmAddress		nextElt;
	ClProtocol		protocol;
	char			*hostName;
	unsigned short		portNbr;
	unsigned int		hostNbr;
	struct sockaddr_in	*inetName;
	Object			bundleZco;
	BpAncillaryData		ancillaryData;
	unsigned int		bundleLength;
	unsigned char		*buffer;

	if (ductName == NULL)
	{
		PUTS("Usage: udpmoondelayclo <remote host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putErrmsg("udpmoondelayclo can't attach to BP.", NULL);
		return -1;
	}

	findOutduct("udp", ductName, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such udp duct.", ductName);
		return -1;
	}
	
	/* Read outduct and plan information like original udpclo */
	neighbor = NULL;
	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));
	sdr_read(sdr, (char *) &outduct, sdr_list_data(sdr, vduct->outductElt),
			sizeof(Outduct));
	if (outduct.planDuctListElt)
	{
		planDuctList = sdr_list_list(sdr, outduct.planDuctListElt);
		planObj = sdr_list_user_data(sdr, planDuctList);
		if (planObj)
		{
			sdr_read(sdr, (char *) &plan, planObj, sizeof(BpPlan));
		}
	}
	sdr_exit_xn(sdr);

	if (vduct->cloPid != ERROR && vduct->cloPid != sm_TaskIdSelf())
	{
		if (sm_TaskExists(vduct->cloPid))
		{
			putErrmsg("CLO task is already started for this duct.",
					itoa(vduct->cloPid));
			return -1;
		}
		else
		{
			writeMemo("[i] Clearing stale CLO PID for duct.");
			vduct->cloPid = ERROR;
		}
	}

	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));
	sdr_read(sdr, (char *) &outduct, sdr_list_data(sdr, vduct->outductElt),
			sizeof(Outduct));
	sdr_read(sdr, (char *) &protocol, outduct.protocol, sizeof(ClProtocol));
	sdr_exit_xn(sdr);
	hostName = ductName;
	parseSocketSpec(ductName, &portNbr, &hostNbr);
	if (portNbr == 0)
	{
		portNbr = BpUdpDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	hostNbr = htonl(hostNbr);
	memset((char *) &socketName, 0, sizeof socketName);
	inetName = (struct sockaddr_in *) &socketName;
	inetName->sin_family = AF_INET;
	inetName->sin_port = portNbr;
	memcpy((char *) &(inetName->sin_addr.s_addr), (char *) &hostNbr, 4);
	ductSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (ductSocket < 0)
	{
		putSysErrmsg("Can't open UDP socket", NULL);
		return -1;
	}

	/* Initialize random number generator for link loss simulation */
	srand((unsigned int)time(NULL));
	
	/* Initialize bundle queue */
	initQueue();
	
	/* Set up signal handling for clean shutdown */
	oK(udpmoondelaycloSemaphore(&(vduct->semaphore)));
	isignal(SIGTERM, shutDownClo);
	
	/* Register this CLO with the vduct */
	vduct->cloPid = sm_TaskIdSelf();
	
	/* Allocate send buffer */
	buffer = MTAKE(UDPCLA_BUFSZ);
	globalBuffer = buffer; /* Store for monitor thread access */
	if (buffer == NULL)
	{
		putErrmsg("udpmoondelayclo can't get UDP buffer.", NULL);
		destroyQueue();
		closesocket(ductSocket);
		return -1;
	}

	/* Can now start sending bundles. */
	{
		char	memoBuf[256];
		double	currentDelay = calculateMoonDelay();

		isprintf(memoBuf, sizeof(memoBuf),
				"[i] udpmoondelayclo is running, spec = '%s', Moon delay = %.1f sec, link loss = %.1f%% (continuous monitoring thread).",
				ductName, currentDelay, LINK_LOSS_PERCENTAGE);
		writeMemo(memoBuf);
	}

	/* Start continuous queue monitoring thread */
	if (pthread_create(&monitorThread, NULL, queueMonitorThread, NULL) != 0) {
		putErrmsg("Can't create monitor thread.", NULL);
		MRELEASE(buffer);
		destroyQueue();
		closesocket(ductSocket);
		return -1;
	}
	
	writeMemo("[DEBUG] udpmoondelayclo: Monitor thread created, starting ION dequeue loop");

	/* Main processing loop - ION interface only (monitor thread handles sending) */
	while (g_running)
	{
		/* Try to dequeue a bundle from ION (blocking with timeout) */
		if (bpDequeue(vduct, &bundleZco, &ancillaryData, 1000) < 0)
		{
			putErrmsg("Can't dequeue bundle.", NULL);
			break;
		}
		
		if (bundleZco == 0)	/*	No bundle available (timeout).		*/
		{
			/* Monitor thread handles sending, just continue */
			continue;
		}

		if (bundleZco == 1)	/*	Got a corrupt bundle.	*/
		{
			continue;	/*	Get next bundle.	*/
		}
		
		/* Valid bundle received */
		{
			/* Debug: Log that we received a bundle */
			writeMemo("[DEBUG] udpmoondelayclo: Received bundle from ION");
			/* Get bundle length from ZCO */
			CHKZERO(sdr_begin_xn(sdr));
			bundleLength = zco_length(sdr, bundleZco);
			sdr_exit_xn(sdr);
			
			/* Add bundle to queue for delayed sending */
			if (addBundle(bundleZco, &ancillaryData, bundleLength) < 0) {
				putErrmsg("Can't queue bundle - queue full.", NULL);
				/* Still need to clean up the ZCO */
				CHKZERO(sdr_begin_xn(sdr));
				zco_destroy(sdr, bundleZco);
				sdr_exit_xn(sdr);
			}
		}
	}

	/* Stop processing */
	g_running = 0;
	
	/* Wait for monitor thread to finish */
	writeMemo("[DEBUG] udpmoondelayclo: Waiting for monitor thread to finish");
	pthread_join(monitorThread, NULL);

	/* Clear CLO PID from vduct */
	if (vduct->cloPid == sm_TaskIdSelf())
	{
		vduct->cloPid = ERROR;
	}
	
	closesocket(ductSocket);
	MRELEASE(buffer);
	destroyQueue();
	writeErrmsgMemos();
	writeMemo("[i] udpmoondelayclo duct has ended.");
	ionDetach();
	return 0;
}