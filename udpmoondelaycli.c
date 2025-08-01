/*
	udpmoondelaycli.c:	UDP Moon Delay convergence-layer input daemon
				with parallel bundle processing and link loss simulation.

	Based on original ION UDP convergence layer (udpcli.c)
	Author: Samo Grasic (samo@grasic.net), LateLab AB, Sweden

	Copyright (c) 2006, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship acknowledged.
*/

#include "udpcla.h"
#include "ipnfw.h"
#include "dtn2fw.h"

/* Moon delay constants */
#define SPEED_OF_LIGHT 299792.458     /* km/s */
#define MOON_AVG_DISTANCE 384400.0    /* km */
#define MOON_VARIATION 20000.0        /* km, variation range */



/* Link loss simulation - can be modified at compile time */
#ifndef LINK_LOSS_PERCENTAGE
#define LINK_LOSS_PERCENTAGE 0.0  /* 0.0 = no loss, 5.0 = 5% loss */
#endif

/* Global running flag for main thread */
static int g_running = 1;

/* Buffer management for timed bundle processing */
#define MAX_BUFFERED_BUNDLES 100
#define BUNDLE_BUFFER_SIZE UDPCLA_BUFSZ

typedef struct {
	char *data;
	int length;
	struct sockaddr_in fromAddr;
	struct timeval arrivalTime;
	struct timeval processTime;  /* When to process this bundle */
	double delaySeconds;
	int processed;               /* Flag to mark as processed */
} TimedBundle;

typedef struct {
	TimedBundle bundles[MAX_BUFFERED_BUNDLES];
	int head;
	int tail;
	int count;
	pthread_mutex_t mutex;
	pthread_cond_t notEmpty;
	pthread_cond_t notFull;
	int running;
} TimedBundleQueue;


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

/* Calculate Moon delay based on current orbital position */
static double calculateMoonDelay(void)
{
	time_t now = time(NULL);
	double moonPhase;
	double distance;
	
	/* Simple lunar orbit model - Moon completes orbit in ~27.3 days */
	moonPhase = fmod((double)(now / 86400.0) * 2.0 * M_PI / 27.3, 2.0 * M_PI);
	
	/* Calculate distance with sinusoidal variation */
	distance = MOON_AVG_DISTANCE + MOON_VARIATION * sin(moonPhase);
	
	/* Convert to light-travel time */
	return distance / SPEED_OF_LIGHT;
}

/* Initialize timed bundle queue */
static void initTimedBundleQueue(TimedBundleQueue *queue)
{
	queue->head = 0;
	queue->tail = 0;
	queue->count = 0;
	queue->running = 1;
	pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->notEmpty, NULL);
	pthread_cond_init(&queue->notFull, NULL);
	
	/* Initialize all bundle slots */
	for (int i = 0; i < MAX_BUFFERED_BUNDLES; i++) {
		queue->bundles[i].data = NULL;
		queue->bundles[i].processed = 1; /* Mark as processed (empty) */
	}
}

/* Destroy timed bundle queue */
static void destroyTimedBundleQueue(TimedBundleQueue *queue)
{
	pthread_mutex_lock(&queue->mutex);
	queue->running = 0;
	
	/* Free any remaining buffered data */
	for (int i = 0; i < MAX_BUFFERED_BUNDLES; i++) {
		if (queue->bundles[i].data && !queue->bundles[i].processed) {
			MRELEASE(queue->bundles[i].data);
			queue->bundles[i].data = NULL;
		}
	}
	
	pthread_cond_broadcast(&queue->notEmpty);
	pthread_cond_broadcast(&queue->notFull);
	pthread_mutex_unlock(&queue->mutex);
	
	pthread_mutex_destroy(&queue->mutex);
	pthread_cond_destroy(&queue->notEmpty);
	pthread_cond_destroy(&queue->notFull);
}

/* Add bundle to timed queue with calculated process time */
static int enqueueTimedBundle(TimedBundleQueue *queue, const char *data, int length, 
			      const struct sockaddr_in *fromAddr)
{
	pthread_mutex_lock(&queue->mutex);
	
	/* Wait if queue is full */
	while (queue->count >= MAX_BUFFERED_BUNDLES && queue->running) {
		pthread_cond_wait(&queue->notFull, &queue->mutex);
	}
	
	if (!queue->running) {
		pthread_mutex_unlock(&queue->mutex);
		return -1;
	}
	
	TimedBundle *bundle = &queue->bundles[queue->tail];
	
	/* Allocate and copy data */
	bundle->data = MTAKE(length);
	if (bundle->data == NULL) {
		pthread_mutex_unlock(&queue->mutex);
		return -1;
	}
	
	memcpy(bundle->data, data, length);
	bundle->length = length;
	bundle->fromAddr = *fromAddr;
	gettimeofday(&bundle->arrivalTime, NULL);
	bundle->processed = 0;  /* Mark as not processed */
	
	/* Calculate delay for this specific bundle */
	bundle->delaySeconds = calculateMoonDelay();
	
	/* Calculate when to process this bundle */
	bundle->processTime = bundle->arrivalTime;
	long long delayMicroseconds = (long long)(bundle->delaySeconds * 1000000.0);
	bundle->processTime.tv_usec += delayMicroseconds;
	
	/* Handle overflow */
	while (bundle->processTime.tv_usec >= 1000000) {
		bundle->processTime.tv_sec++;
		bundle->processTime.tv_usec -= 1000000;
	}
	
	queue->tail = (queue->tail + 1) % MAX_BUFFERED_BUNDLES;
	queue->count++;
	
	pthread_cond_signal(&queue->notEmpty);
	pthread_mutex_unlock(&queue->mutex);
	
	return 0;
}

/* Get the next ready bundle (non-blocking check) */
static TimedBundle* getReadyBundle(TimedBundleQueue *queue)
{
	struct timeval now;
	TimedBundle* readyBundle = NULL;
	
	gettimeofday(&now, NULL);
	
	pthread_mutex_lock(&queue->mutex);
	
	/* Check all bundles to see if any are ready */
	for (int i = 0; i < MAX_BUFFERED_BUNDLES; i++) {
		TimedBundle *bundle = &queue->bundles[i];
		
		/* Skip empty slots or already processed bundles */
		if (bundle->data == NULL || bundle->processed) {
			continue;
		}
		
		/* Check if this bundle is ready to be processed */
		if (now.tv_sec > bundle->processTime.tv_sec ||
		    (now.tv_sec == bundle->processTime.tv_sec && 
		     now.tv_usec >= bundle->processTime.tv_usec)) {
			
			/* Mark as processed and return it */
			bundle->processed = 1;
			queue->count--;
			readyBundle = bundle;
			
			/* Signal waiting threads that space is available */
			pthread_cond_signal(&queue->notFull);
			break;
		}
	}
	
	pthread_mutex_unlock(&queue->mutex);
	return readyBundle;
}


/* Process a timed bundle (after delay has elapsed) */
static int processTimedBundle(AcqWorkArea *work, TimedBundle *bundle)
{
	unsigned int hostNbr;
	char hostName[MAXHOSTNAMELEN + 1];
	
	/* Check for link loss after delay - randomly drop bundle */
	if (shouldDropBundle()) {
		/* Simulate bundle loss - just return without processing */
		MRELEASE(bundle->data);
		bundle->data = NULL;
		return 0;  /* Successfully "processed" (dropped) */
	}
	
	/* Process the bundle */
	memcpy((char *) &hostNbr, (char *) &(bundle->fromAddr.sin_addr.s_addr), 4);
	hostNbr = ntohl(hostNbr);
	printDottedString(hostNbr, hostName);
	
	if (bpBeginAcq(work, 0, NULL) < 0
	|| bpContinueAcq(work, bundle->data, bundle->length, 0, 0) < 0
	|| bpEndAcq(work) < 0)
	{
		putErrmsg("Can't acquire bundle.", hostName);
		MRELEASE(bundle->data);
		bundle->data = NULL;
		return -1;
	}
	
	/* Free the bundle data */
	MRELEASE(bundle->data);
	bundle->data = NULL;
	
	return 0;
}

typedef struct
{
	VInduct			*vduct;
	int			ductSocket;
	int			running;
	TimedBundleQueue	*queue;
} ReceiverThreadParms;


static void	interruptThread(int signum)
{
	isignal(SIGTERM, interruptThread);
	g_running = 0;
	ionKillMainThread("udpmoondelaycli");
}

static void	*handleDatagrams(void *parm)
{
	/*	Main loop for UDP datagram reception - queues bundles with timing	*/

	ReceiverThreadParms	*rtp = (ReceiverThreadParms *) parm;
	char			*procName = "udpmoondelaycli";
	char			*buffer;
	int			bundleLength;
	struct sockaddr_in	fromAddr;

	snooze(1);	/*	Let main thread become interruptible.	*/

	buffer = MTAKE(UDPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udpmoondelaycli can't get UDP buffer.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}

	/*	Continuously receive bundles and queue them with timing info	*/

	while (rtp->running)
	{	
		bundleLength = receiveBytesByUDP(rtp->ductSocket, &fromAddr,
				buffer, UDPCLA_BUFSZ);
		switch (bundleLength)
		{
		case -1:
		case 0:
			putErrmsg("Can't acquire bundle.", NULL);
			ionKillMainThread(procName);

			/*	Intentional fall-through to next case.	*/

		case 1:				/*	Normal stop.	*/
			rtp->running = 0;
			continue;

		default:
			break;			/*	Out of switch.	*/
		}

		/* Queue bundle with calculated processing time - NON-BLOCKING */
		if (enqueueTimedBundle(rtp->queue, buffer, bundleLength, &fromAddr) < 0) {
			putErrmsg("Can't queue timed bundle - queue full.", NULL);
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/
		sm_TaskYield();
	}

	writeErrmsgMemos();
	writeMemo("[i] udpmoondelaycli receiver thread has ended.");

	/*	Free resources.						*/
	MRELEASE(buffer);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (ION_LWT)
int	udpmoondelaycli(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char	*ductName = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*ductName = (argc > 1 ? argv[1] : NULL);
#endif
	VInduct			*vduct;
	PsmAddress		vductElt;
	Sdr			sdr;
	Induct			duct;
	ClProtocol		protocol;
	char			*hostName;
	unsigned short		portNbr;
	unsigned int		hostNbr;
	struct sockaddr		socketName;
	struct sockaddr_in	*inetName;
	socklen_t		nameLength;
	ReceiverThreadParms	rtp;
	pthread_t		receiverThread;
	TimedBundleQueue	timedQueue;
	AcqWorkArea		*work;
	int			fd;
	int			i;
	char			quit = 0;

	if (ductName == NULL)
	{
		PUTS("Usage: udpmoondelaycli <local host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putErrmsg("udpmoondelaycli can't attach to BP.", NULL);
		return -1;
	}

	/* Initialize random number generator for link loss simulation */
	srand((unsigned int)time(NULL));

	findInduct("udp", ductName, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such udp duct.", ductName);
		return -1;
	}

	if (vduct->cliPid != ERROR && vduct->cliPid != sm_TaskIdSelf())
	{
		putErrmsg("CLI task is already started for this duct.",
				itoa(vduct->cliPid));
		return -1;
	}

	/*	All command-line arguments are now validated.		*/

	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));
	sdr_read(sdr, (char *) &duct, sdr_list_data(sdr, vduct->inductElt),
			sizeof(Induct));
	sdr_read(sdr, (char *) &protocol, duct.protocol, sizeof(ClProtocol));
	sdr_exit_xn(sdr);
	hostName = ductName;
	if (parseSocketSpec(ductName, &portNbr, &hostNbr) != 0)
	{
		putErrmsg("Can't get IP/port for host.", hostName);
		return -1;
	}

	if (portNbr == 0)
	{
		portNbr = BpUdpDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	hostNbr = htonl(hostNbr);
	rtp.vduct = vduct;
	memset((char *) &socketName, 0, sizeof socketName);
	inetName = (struct sockaddr_in *) &socketName;
	inetName->sin_family = AF_INET;
	inetName->sin_port = portNbr;
	memcpy((char *) &(inetName->sin_addr.s_addr), (char *) &hostNbr, 4);
	rtp.ductSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.ductSocket < 0)
	{
		putSysErrmsg("Can't open UDP socket", NULL);
		return -1;
	}

	nameLength = sizeof(struct sockaddr);
	if (reUseAddress(rtp.ductSocket)
	|| bind(rtp.ductSocket, &socketName, nameLength) < 0
	|| getsockname(rtp.ductSocket, &socketName, &nameLength) < 0)
	{
		closesocket(rtp.ductSocket);
		putSysErrmsg("Can't initialize socket", NULL);
		return -1;
	}

	/* Initialize timed bundle queue */
	initTimedBundleQueue(&timedQueue);
	rtp.queue = &timedQueue;
	rtp.running = 1;
	
	/* Get acquisition work area for main thread bundle processing */
	work = bpGetAcqArea(vduct);
	if (work == NULL)
	{
		putErrmsg("udpmoondelaycli can't get acquisition work area.", NULL);
		destroyTimedBundleQueue(&timedQueue);
		return -1;
	}

	/*	Set up signal handling; SIGTERM is shutdown signal.	*/

	ionNoteMainThread("udpmoondelaycli");
	isignal(SIGTERM, interruptThread);

	/*	Start the receiver thread.		*/

	if (pthread_begin(&receiverThread, NULL, handleDatagrams, &rtp))
	{
		closesocket(rtp.ductSocket);
		putSysErrmsg("udpmoondelaycli can't create receiver thread", NULL);
		return -1;
	}

	/*	Main processing loop - check for ready bundles	*/

	{
		char	txt[500];
		double	currentDelay = calculateMoonDelay();

		isprintf(txt, sizeof(txt),
			"[i] udpmoondelaycli is running, spec=[%s:%d], Moon delay = %.1f sec, link loss = %.1f%% (timed Moon processing).", 
			inet_ntoa(inetName->sin_addr), ntohs(portNbr), currentDelay, LINK_LOSS_PERCENTAGE);
		writeMemo(txt);
	}


	/*	Main processing loop - check for ready bundles	*/
	
	while (g_running && rtp.running) {
		TimedBundle *readyBundle = getReadyBundle(&timedQueue);
		
		if (readyBundle != NULL) {
			/* Process this bundle now that its delay has elapsed */
			if (processTimedBundle(work, readyBundle) < 0) {
				putErrmsg("Can't process timed bundle.", NULL);
				g_running = 0;
				rtp.running = 0;
				break;
			}
		} else {
			/* No ready bundles, sleep briefly and check again */
			microsnooze(10000);  /* Sleep 10ms */
		}
	}

	/*	Time to shut down.					*/

	rtp.running = 0;

	/*	Wake up the receiver thread by sending a quit byte.	*/

	if (hostNbr == 0)	/*	Receiving on INADDR_ANY.	*/
	{
		hostNbr = (127 << 24) + 1;	/*	127.0.0.1	*/
		hostNbr = htonl(hostNbr);
		memcpy((char *) &(inetName->sin_addr.s_addr),
				(char *) &hostNbr, 4);
	}

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd >= 0)
	{
		for (i = 0; i < 3; i++)
		{
			oK(isendto(fd, &quit, 1, 0, &socketName,
					sizeof(struct sockaddr)));
			microsnooze(250000);

			if (pthread_kill(receiverThread, SIGCONT) != 0)
			{
				break;
			}
		}
		closesocket(fd);
	}

	pthread_detach(receiverThread);
	closesocket(rtp.ductSocket);
	bpReleaseAcqArea(work);
	destroyTimedBundleQueue(&timedQueue);
	writeErrmsgMemos();
	writeMemo("[i] udpmoondelaycli has ended.");
	ionDetach();
	return 0;
}