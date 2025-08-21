/*
	udpmarsdelaycli.c:	UDP Mars Delay convergence-layer input daemon
				with simplified single-threaded queue processing and link loss simulation.

	Based on original ION UDP convergence layer (udpcli.c)
	Author: Samo Grasic (samo@grasic.net), LateLab AB, Sweden

	Copyright (c) 2006, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship acknowledged.
*/

#include "udpcla.h"
#include "ipnfw.h"
#include "dtn2fw.h"
#include <fcntl.h>
#include <errno.h>

/* Mars delay constants */
#define SPEED_OF_LIGHT 299792.458          /* km/s */
#define EARTH_ORBITAL_RADIUS  149598000.0  /* km, 1 AU */
#define MARS_ORBITAL_RADIUS   227939200.0  /* km, 1.52 AU */

/* Link loss simulation - can be modified at compile time */
#ifndef LINK_LOSS_PERCENTAGE
#define LINK_LOSS_PERCENTAGE 0.0  /* 0.0 = no loss, 5.0 = 5% loss */
#endif

/* Simple bundle queue management - single threaded */
#define MAX_QUEUED_BUNDLES 100

typedef struct {
	char *data;
	int length;
	struct sockaddr_in fromAddr;
	struct timeval processTime;  /* When to process this bundle */
} QueuedBundle;

typedef struct {
	QueuedBundle bundles[MAX_QUEUED_BUNDLES];
	int count;
} BundleQueue;

static BundleQueue queue;
static int g_running = 1;

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

/* Calculate Mars delay based on current orbital positions */
static double calculateMarsDelay(void)
{
	time_t now = time(NULL);
	double earthAngle, marsAngle;
	double earthX, earthY, marsX, marsY;
	double distance;
	
	/* Simple orbital model - Earth completes orbit in ~365 days, Mars in ~687 days */
	earthAngle = fmod((double)(now / 86400.0) * 2.0 * M_PI / 365.25, 2.0 * M_PI);
	marsAngle = fmod((double)(now / 86400.0) * 2.0 * M_PI / 687.0, 2.0 * M_PI);
	
	/* Calculate positions */
	earthX = EARTH_ORBITAL_RADIUS * cos(earthAngle);
	earthY = EARTH_ORBITAL_RADIUS * sin(earthAngle);
	marsX = MARS_ORBITAL_RADIUS * cos(marsAngle);
	marsY = MARS_ORBITAL_RADIUS * sin(marsAngle);
	
	/* Calculate distance */
	distance = sqrt((marsX - earthX) * (marsX - earthX) + 
			(marsY - earthY) * (marsY - earthY));
	
	/* Convert to light-travel time */
	return distance / SPEED_OF_LIGHT;
}

/* Initialize bundle queue */
static void initQueue(void)
{
	memset(&queue, 0, sizeof(BundleQueue));
}

/* Add bundle to queue */
static int addBundle(char *data, int length, struct sockaddr_in *fromAddr)
{
	if (queue.count >= MAX_QUEUED_BUNDLES) {
		return -1;  /* Queue full */
	}
	
	QueuedBundle *bundle = &queue.bundles[queue.count];
	
	/* Allocate and copy data */
	bundle->data = MTAKE(length);
	if (bundle->data == NULL) {
		return -1;
	}
	
	memcpy(bundle->data, data, length);
	bundle->length = length;
	bundle->fromAddr = *fromAddr;
	
	/* Calculate process time = current time + delay */
	struct timeval now;
	gettimeofday(&now, NULL);
	bundle->processTime = now;
	double delaySeconds = calculateMarsDelay();
	long long delayMicroseconds = (long long)(delaySeconds * 1000000.0);
	bundle->processTime.tv_usec += delayMicroseconds;
	
	/* Handle overflow */
	while (bundle->processTime.tv_usec >= 1000000) {
		bundle->processTime.tv_sec++;
		bundle->processTime.tv_usec -= 1000000;
	}
	
	queue.count++;
	return 0;
}

/* Process a bundle (after delay has elapsed) */
static int processBundle(AcqWorkArea *work, QueuedBundle *bundle, char *hostName)
{
	/* Check for link loss */
	if (shouldDropBundle()) {
		/* Simulate bundle loss - just drop it */
		return 0;
	}
	
	if (bpBeginAcq(work, 0, NULL) < 0)
	{
		putErrmsg("Can't begin bundle acquisition.", hostName);
		return -1;
	}
	
	if (bpContinueAcq(work, bundle->data, bundle->length, 0, 0) < 0)
	{
		putErrmsg("Can't continue bundle acquisition.", hostName);
		bpCancelAcq(work);
		return -1;
	}
	
	if (bpEndAcq(work) < 0)
	{
		putErrmsg("Can't end bundle acquisition.", hostName);
		return -1;
	}
	
	return 0;
}

/* Process ready bundles and wait for exact timing */
static void processReadyBundles(AcqWorkArea *work)
{
	struct timeval now;
	int processed = 0;
	
	for (int i = 0; i < queue.count; i++) {
		QueuedBundle *bundle = &queue.bundles[i];
		
		/* Check if this bundle is ready to be processed */
		gettimeofday(&now, NULL);
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
			if (processBundle(work, bundle, hostName) < 0) {
				putErrmsg("Can't process bundle.", NULL);
			}
			
			/* Free the data */
			if (bundle->data) {
				MRELEASE(bundle->data);
				bundle->data = NULL;
			}
			
			/* Mark for removal */
			bundle->length = 0;
			processed++;
		}
	}
	
	/* Remove processed bundles by compacting array */
	if (processed > 0) {
		int writeIndex = 0;
		for (int readIndex = 0; readIndex < queue.count; readIndex++) {
			if (queue.bundles[readIndex].length > 0) {
				if (writeIndex != readIndex) {
					queue.bundles[writeIndex] = queue.bundles[readIndex];
				}
				writeIndex++;
			}
		}
		queue.count = writeIndex;
	}
}


/* Cleanup queue */
static void destroyQueue(void)
{
	/* Free any remaining data */
	for (int i = 0; i < queue.count; i++) {
		if (queue.bundles[i].data) {
			MRELEASE(queue.bundles[i].data);
		}
	}
	queue.count = 0;
}

static void interruptThread(int signum)
{
	isignal(SIGTERM, interruptThread);
	isignal(SIGINT, interruptThread);
	isignal(SIGHUP, interruptThread);
	g_running = 0;
	writeMemo("[i] udpmarsdelaycli received shutdown signal, terminating gracefully...");
	ionKillMainThread("udpmarsdelaycli");
}

#if defined (ION_LWT)
int	udpmarsdelaycli(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
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
	int				ductSocket;
	AcqWorkArea		*work;
	char			*buffer;
	int				bundleLength;
	struct sockaddr_in	fromAddr;

	if (endpointSpec == NULL)
	{
		PUTS("Usage: udpmarsdelaycli <local host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putErrmsg("udpmarsdelaycli can't attach to BP.", NULL);
		return -1;
	}

	findInduct("udp", endpointSpec, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such udp duct.", endpointSpec);
		return -1;
	}

	/* Enhanced process check with cleanup for stale PIDs */
	if (vduct->cliPid != ERROR && vduct->cliPid != sm_TaskIdSelf())
	{
		/* Check if the PID is actually running */
		if (sm_TaskExists(vduct->cliPid))
		{
			putErrmsg("CLI task is already started for this duct.",
					itoa(vduct->cliPid));
			return -1;
		}
		else
		{
			/* Stale PID - clear it and continue */
			writeMemo("[i] Clearing stale CLI PID for duct.");
			vduct->cliPid = ERROR;
		}
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

	/* Enhanced socket options for better restart behavior */
	int optval = 1;
	if (setsockopt(ductSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
	{
		putSysErrmsg("Can't set SO_REUSEADDR", NULL);
	}
	
#ifdef SO_REUSEPORT
	if (setsockopt(ductSocket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0)
	{
		/* SO_REUSEPORT not critical - continue without error */
		writeMemo("[w] SO_REUSEPORT not available, continuing.");
	}
#endif

	if (bind(ductSocket, &socketName, sizeof(struct sockaddr_in)) < 0)
	{
		closesocket(ductSocket);
		putSysErrmsg("Can't initialize socket", NULL);
		return -1;
	}

	work = bpGetAcqArea(vduct);
	if (work == NULL)
	{
		putErrmsg("udpmarsdelaycli can't get acquisition work area.", NULL);
		closesocket(ductSocket);
		return -1;
	}

	/* Initialize random number generator for link loss simulation */
	srand((unsigned int)time(NULL));
	
	/* Initialize bundle queue */
	initQueue();

	/* Set up signal handling for clean shutdown */
	ionNoteMainThread("udpmarsdelaycli");
	isignal(SIGTERM, interruptThread);
	isignal(SIGINT, interruptThread);
	isignal(SIGHUP, interruptThread);
	
	/* Register this CLI with the vduct */
	vduct->cliPid = sm_TaskIdSelf();

	/* Allocate receive buffer */
	buffer = MTAKE(UDPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udpmarsdelaycli can't get UDP buffer.", NULL);
		destroyQueue();
		closesocket(ductSocket);
		return -1;
	}

	/* Can now start receiving bundles. */
	{
		char	memoBuf[256];
		double	currentDelay = calculateMarsDelay();

		isprintf(memoBuf, sizeof(memoBuf),
				"[i] udpmarsdelaycli is running, spec=[%s:%d], Mars delay = %.1f sec, link loss = %.1f%% (single-threaded queue).",
				hostName, ntohs(portNbr), currentDelay, LINK_LOSS_PERCENTAGE);
		writeMemo(memoBuf);
	}

	/* Main processing loop - single threaded with select() for non-blocking behavior */
	while (g_running)
	{
		fd_set readfds;
		struct timeval timeout;
		int selectResult;
		
		/* Set up select() to check for data availability with short timeout */
		FD_ZERO(&readfds);
		FD_SET(ductSocket, &readfds);
		timeout.tv_sec = 0;
		timeout.tv_usec = 1000;  /* 1ms timeout */
		
		selectResult = select(ductSocket + 1, &readfds, NULL, NULL, &timeout);
		
		if (selectResult > 0 && FD_ISSET(ductSocket, &readfds)) {
			/* Data available - try to receive a bundle */
			bundleLength = receiveBytesByUDP(ductSocket, &fromAddr, buffer, UDPCLA_BUFSZ);
			
			if (bundleLength > 1) {
				/* Add bundle to queue for delayed processing */
				if (addBundle(buffer, bundleLength, &fromAddr) < 0) {
					putErrmsg("Can't queue bundle - queue full.", NULL);
				}
			} else if (bundleLength == 1) {
				/* Normal stop signal */
				g_running = 0;
			} else if (bundleLength < 0) {
				/* Error receiving bundle */
				putErrmsg("Can't receive bundle.", NULL);
				g_running = 0;
			}
		} else if (selectResult < 0) {
			/* select() error - check if interrupted by signal */
			if (errno == EINTR) {
				/* Interrupted by signal during shutdown - this is normal */
				continue;
			}
			putSysErrmsg("Can't select on UDP socket", NULL);
			g_running = 0;
		}
		/* selectResult == 0 means timeout - just continue to process ready bundles */
		
		/* Process ready bundles */
		processReadyBundles(work);
	}

	/* Clear CLI PID from vduct */
	if (vduct->cliPid == sm_TaskIdSelf())
	{
		vduct->cliPid = ERROR;
	}
	closesocket(ductSocket);
	MRELEASE(buffer);
	bpReleaseAcqArea(work);
	destroyQueue();
	writeErrmsgMemos();
	writeMemo("[i] udpmarsdelaycli duct has ended.");
	ionDetach();
	return 0;
}