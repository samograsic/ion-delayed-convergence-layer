/*
	udppresetdelaycli.c:	UDP Preset Delay convergence-layer input daemon
				with timed bundle processing and link loss simulation.

	Based on original ION UDP convergence layer (udpcli.c)
	Author: Samo Grasic (samo@grasic.net), LateLab AB, Sweden

	Copyright (c) 2006, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship acknowledged.
*/

#include "udpcla.h"

/* Preset delay in seconds - can be modified at compile time */
#ifndef PRESET_DELAY_SECONDS
#define PRESET_DELAY_SECONDS 10.0
#endif

/* Link loss simulation - can be modified at compile time */
#ifndef LINK_LOSS_PERCENTAGE
#define LINK_LOSS_PERCENTAGE 0.0  /* 0.0 = no loss, 5.0 = 5% loss */
#endif

/* Timed bundle queue management */
#define MAX_TIMED_BUNDLES 1000

typedef struct {
	char *data;
	int length;
	struct sockaddr_in fromAddr;
	struct timeval arrivalTime;
	struct timeval processTime;  /* When to process this bundle */
	int processed;               /* Flag to mark as processed */
} TimedBundle;

typedef struct {
	TimedBundle bundles[MAX_TIMED_BUNDLES];
	int count;
	pthread_mutex_t mutex;
	int running;
} TimedBundleQueue;

static TimedBundleQueue timedQueue;

/* Forward declarations */
static void processReadyBundles(AcqWorkArea *work);

/* Receiver thread parameters structure */
typedef struct {
	int linkSocket;
	VInduct *vduct;
	int running;
} ReceiverThreadParms;

/* Bundle processor thread parameters */
typedef struct {
	AcqWorkArea *work;
	int running;
} ProcessorThreadParms;

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

/* Get preset delay */
static double getPresetDelay(void)
{
	return PRESET_DELAY_SECONDS;
}

/* Initialize timed bundle queue */
static void initTimedQueue(void)
{
	memset(&timedQueue, 0, sizeof(TimedBundleQueue));
	pthread_mutex_init(&timedQueue.mutex, NULL);
	timedQueue.running = 1;
}

/* Add bundle to timed queue */
static int addTimedBundle(char *data, int length, struct sockaddr_in *fromAddr)
{
	pthread_mutex_lock(&timedQueue.mutex);
	
	if (timedQueue.count >= MAX_TIMED_BUNDLES) {
		pthread_mutex_unlock(&timedQueue.mutex);
		return -1;  /* Queue full */
	}
	
	TimedBundle *bundle = &timedQueue.bundles[timedQueue.count];
	
	/* Allocate and copy data */
	bundle->data = MTAKE(length);
	if (bundle->data == NULL) {
		pthread_mutex_unlock(&timedQueue.mutex);
		return -1;
	}
	
	memcpy(bundle->data, data, length);
	bundle->length = length;
	bundle->fromAddr = *fromAddr;
	gettimeofday(&bundle->arrivalTime, NULL);
	bundle->processed = 0;
	
	/* Calculate process time = arrival time + delay */
	bundle->processTime = bundle->arrivalTime;
	double delaySeconds = getPresetDelay();
	long long delayMicroseconds = (long long)(delaySeconds * 1000000.0);
	bundle->processTime.tv_usec += delayMicroseconds;
	
	/* Handle overflow */
	while (bundle->processTime.tv_usec >= 1000000) {
		bundle->processTime.tv_sec++;
		bundle->processTime.tv_usec -= 1000000;
	}
	
	timedQueue.count++;
	pthread_mutex_unlock(&timedQueue.mutex);
	return 0;
}

/* Process a timed bundle (after delay has elapsed) */
static int processTimedBundle(AcqWorkArea *work, TimedBundle *bundle, char *hostName)
{
	/* Check for link loss */
	if (shouldDropBundle()) {
		/* Simulate bundle loss - just drop it */
		return 0;
	}
	
	/* Process bundle using ION's bundle protocol functions - ION handles SDR internally */
	if (bpBeginAcq(work, 0, NULL) < 0
	|| bpContinueAcq(work, bundle->data, bundle->length, 0, 0) < 0
	|| bpEndAcq(work) < 0)
	{
		putErrmsg("Can't acquire bundle.", hostName);
		return -1;
	}
	
	return 0;
}

/* Bundle processor thread */
static void *bundleProcessor(void *parm)
{
	ProcessorThreadParms *ptp = (ProcessorThreadParms *)parm;
	
	while (ptp->running)
	{
		processReadyBundles(ptp->work);
		microsnooze(10000);  /* 10ms */
	}
	
	writeMemo("[i] udppresetdelaycli processor thread has ended.");
	return NULL;
}

/* Process ready bundles (those whose time has come) */
static void processReadyBundles(AcqWorkArea *work)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	
	pthread_mutex_lock(&timedQueue.mutex);
	
	for (int i = 0; i < timedQueue.count; i++) {
		TimedBundle *bundle = &timedQueue.bundles[i];
		
		if (bundle->processed) continue;
		
		/* Check if this bundle is ready to be processed */
		if (now.tv_sec > bundle->processTime.tv_sec ||
		    (now.tv_sec == bundle->processTime.tv_sec && 
		     now.tv_usec >= bundle->processTime.tv_usec)) {
			
			/* Get host name for error reporting */
			unsigned int hostNbr;
			char hostName[MAXHOSTNAMELEN + 1];
			memcpy((char *) &hostNbr, (char *) &(bundle->fromAddr.sin_addr.s_addr), 4);
			hostNbr = ntohl(hostNbr);
			printDottedString(hostNbr, hostName);
			
			/* Process the bundle */
			if (processTimedBundle(work, bundle, hostName) < 0) {
				putErrmsg("Can't process timed bundle.", NULL);
			}
			
			bundle->processed = 1;
		}
	}
	
	/* Clean up processed bundles by compacting the array */
	int writeIndex = 0;
	for (int readIndex = 0; readIndex < timedQueue.count; readIndex++) {
		if (!timedQueue.bundles[readIndex].processed) {
			if (writeIndex != readIndex) {
				timedQueue.bundles[writeIndex] = timedQueue.bundles[readIndex];
			}
			writeIndex++;
		} else {
			/* Free the data for processed bundles */
			if (timedQueue.bundles[readIndex].data) {
				MRELEASE(timedQueue.bundles[readIndex].data);
				timedQueue.bundles[readIndex].data = NULL;
			}
		}
	}
	timedQueue.count = writeIndex;
	
	pthread_mutex_unlock(&timedQueue.mutex);
}


/* Cleanup timed queue */
static void destroyTimedQueue(void)
{
	pthread_mutex_lock(&timedQueue.mutex);
	timedQueue.running = 0;
	
	/* Free any remaining data */
	for (int i = 0; i < timedQueue.count; i++) {
		if (timedQueue.bundles[i].data) {
			MRELEASE(timedQueue.bundles[i].data);
		}
	}
	timedQueue.count = 0;
	
	pthread_mutex_unlock(&timedQueue.mutex);
	pthread_mutex_destroy(&timedQueue.mutex);
}

static void interruptThread(int signum)
{
	isignal(SIGTERM, interruptThread);
	ionKillMainThread("udppresetdelaycli");
}

/* UDP receiver thread */
static void *udpReceiver(void *parm)
{
	ReceiverThreadParms *rtp = (ReceiverThreadParms *)parm;
	char *buffer;
	int bundleLength;
	struct sockaddr_in fromAddr;
	
	buffer = MTAKE(UDPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udppresetdelaycli can't get UDP buffer.", NULL);
		ionKillMainThread("udppresetdelaycli");
		return NULL;
	}
	
	while (timedQueue.running)
	{
		/* fromSize not used by receiveBytesByUDP */
		bundleLength = receiveBytesByUDP(rtp->linkSocket, &fromAddr,
				buffer, UDPCLA_BUFSZ);
		switch (bundleLength)
		{
		case -1:
		case 0:
			putErrmsg("Can't receive bundle.", NULL);
			ionKillMainThread("udppresetdelaycli");
			
			/*	Intentional fall-through to next case.	*/
			
		case 1:				/*	Normal stop.	*/
			timedQueue.running = 0;
			continue;
			
		default:
			break;			/*	Out of switch.	*/
		}
		
		/* Add bundle to timed queue for delayed processing */
		if (addTimedBundle(buffer, bundleLength, &fromAddr) < 0) {
			putErrmsg("Can't queue timed bundle - queue full.", NULL);
		}
	}
	
	MRELEASE(buffer);
	writeMemo("[i] udppresetdelaycli receiver thread has ended.");
	return NULL;
}

#if defined (ION_LWT)
int	udppresetdelaycli(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char	*endpointSpec = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*endpointSpec = (argc > 1 ? argv[1] : NULL);
#endif
	VInduct			*vduct;
	PsmAddress		vductElt;
	Sdr				sdr;
	Induct			induct;
	ClProtocol		protocol;
	char			*hostName;
	unsigned short	portNbr;
	unsigned int	hostNbr;
	struct sockaddr	socketName;
	struct sockaddr_in	*inetName;
	ReceiverThreadParms	rtp;
	ProcessorThreadParms	ptp;
	pthread_t		receiverThread;
	pthread_t		processorThread;
	int				ductSocket;
	AcqWorkArea		*work;

	if (endpointSpec == NULL)
	{
		PUTS("Usage: udppresetdelaycli <local host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putErrmsg("udppresetdelaycli can't attach to BP.", NULL);
		return -1;
	}

	findInduct("udp", endpointSpec, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such udp duct.", endpointSpec);
		return -1;
	}

	if (vduct->cliPid != ERROR && vduct->cliPid != sm_TaskIdSelf())
	{
		putErrmsg("CLI task is already started for this duct.",
				itoa(vduct->cliPid));
		return -1;
	}

	/* All command-line arguments are now validated. */

	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));
	sdr_read(sdr, (char *) &induct, sdr_list_data(sdr, vduct->inductElt),
			sizeof(Induct));
	sdr_read(sdr, (char *) &protocol, induct.protocol, sizeof(ClProtocol));
	sdr_exit_xn(sdr);
	hostName = endpointSpec;
	parseSocketSpec(endpointSpec, &portNbr, &hostNbr);

	if (portNbr == 0)
	{
		portNbr = BpUdpDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	if (hostNbr == 0)		/* Default to local host. */
	{
		hostNbr = getInternetAddress(hostName);
	}

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

	reUseAddress(ductSocket);
	if (bind(ductSocket, &socketName, sizeof(struct sockaddr_in)) < 0)
	{
		closesocket(ductSocket);
		putSysErrmsg("Can't initialize socket", NULL);
		return -1;
	}

	work = bpGetAcqArea(vduct);
	if (work == NULL)
	{
		putErrmsg("udppresetdelaycli can't get acquisition work area.", NULL);
		closesocket(ductSocket);
		return -1;
	}

	/* Initialize random number generator for link loss simulation */
	srand((unsigned int)time(NULL));
	
	/* Initialize timed bundle queue */
	initTimedQueue();

	/* Set up signal handling for clean shutdown */
	ionNoteMainThread("udppresetdelaycli");
	isignal(SIGTERM, interruptThread);

	/* Start receiver thread */
	rtp.linkSocket = ductSocket;
	rtp.vduct = vduct;
	rtp.running = 1;
	if (pthread_begin(&receiverThread, NULL, udpReceiver, &rtp))
	{
		putSysErrmsg("udppresetdelaycli can't create receiver thread", NULL);
		destroyTimedQueue();
		closesocket(ductSocket);
		return -1;
	}

	/* Start bundle processor thread */
	ptp.work = work;
	ptp.running = 1;
	if (pthread_begin(&processorThread, NULL, bundleProcessor, &ptp))
	{
		putSysErrmsg("udppresetdelaycli can't create processor thread", NULL);
		timedQueue.running = 0;
		pthread_join(receiverThread, NULL);
		destroyTimedQueue();
		closesocket(ductSocket);
		return -1;
	}

	/* Can now start receiving bundles. */
	{
		char	memoBuf[256];
		double	currentDelay = getPresetDelay();

		isprintf(memoBuf, sizeof(memoBuf),
				"[i] udppresetdelaycli is running, spec=[%s:%d], preset delay = %.1f sec, link loss = %.1f%% (timed processing).",
				hostName, ntohs(portNbr), currentDelay, LINK_LOSS_PERCENTAGE);
		writeMemo(memoBuf);
	}

	/* Now sleep until interrupted by SIGTERM */
	ionPauseMainThread(-1);

	/* Time to shut down */
	timedQueue.running = 0;
	ptp.running = 0;
	
	/* Create one-use socket for the closing quit byte */
	unsigned int quitHostNbr = hostNbr;
	if (quitHostNbr == 0)	/* Receiving on INADDR_ANY */
	{
		/* Can't send to host number 0, so send to loopback address */
		quitHostNbr = (127 << 24) + 1;	/* 127.0.0.1 */
		quitHostNbr = htonl(quitHostNbr);
		struct sockaddr_in *quitInetName = (struct sockaddr_in *) &socketName;
		memcpy((char *) &(quitInetName->sin_addr.s_addr), (char *) &quitHostNbr, 4);
	}

	/* Wake up the receiver thread by sending a 1-byte datagram */
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd >= 0)
	{
		char quit = 0;
		if (isendto(fd, &quit, 1, 0, &socketName, sizeof(struct sockaddr)) == 1)
		{
			pthread_join(receiverThread, NULL);
		}
		closesocket(fd);
	}
	
	pthread_join(processorThread, NULL);
	closesocket(ductSocket);
	bpReleaseAcqArea(work);
	destroyTimedQueue();
	writeErrmsgMemos();
	writeMemo("[i] udppresetdelaycli duct has ended.");
	ionDetach();
	return 0;
}