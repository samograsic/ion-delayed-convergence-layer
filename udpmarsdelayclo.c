/*
	udpmarsdelayclo.c:	UDP Mars Delay convergence-layer output daemon
				with parallel bundle processing and link loss simulation.

	Based on original ION UDP convergence layer (udpclo.c)
	Author: Samo Grasic (samo@grasic.net), LateLab AB, Sweden

	Copyright (c) 2006, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship acknowledged.
*/

#include "udpcla.h"

/* Mars delay constants */
#define SPEED_OF_LIGHT 299792.458          /* km/s */
#define EARTH_ORBITAL_RADIUS  149598000.0  /* km, 1 AU */
#define MARS_ORBITAL_RADIUS   227939200.0  /* km, 1.52 AU */

/* Buffer management */
#define MAX_BUFFERED_BUNDLES 200
#define BUNDLE_BUFFER_SIZE UDPCLA_BUFSZ
#define MAX_SENDER_THREADS 50

/* Link loss simulation - can be modified at compile time */
#ifndef LINK_LOSS_PERCENTAGE
#define LINK_LOSS_PERCENTAGE 0.0  /* 0.0 = no loss, 5.0 = 5% loss */
#endif

typedef struct {
	Object bundleZco;
	BpAncillaryData ancillaryData;
	unsigned int bundleLength;
	struct timeval arrivalTime;
	double delaySeconds;
	struct timeval sendTime;  /* When to send this bundle */
	int processed;            /* Flag to mark as processed */
} BufferedBundle;

typedef struct {
	BufferedBundle bundles[MAX_BUFFERED_BUNDLES];
	int head;
	int tail;
	int count;
	pthread_mutex_t mutex;
	pthread_cond_t notEmpty;
	pthread_cond_t notFull;
	int running;
} BundleQueue;

typedef struct {
	BufferedBundle *bundle;
	struct sockaddr *socketName;
	int *ductSocket;
	unsigned char *buffer;
} SenderThreadData;

typedef struct {
	VOutduct *vduct;
	BundleQueue *queue;
	int running;
	struct sockaddr *socketName;
	int *ductSocket;
	unsigned char *buffer;
} OutductThreadParms;

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

static void initBundleQueue(BundleQueue *queue)
{
	queue->head = 0;
	queue->tail = 0;
	queue->count = 0;
	queue->running = 1;
	pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->notEmpty, NULL);
	pthread_cond_init(&queue->notFull, NULL);
}

static void destroyBundleQueue(BundleQueue *queue)
{
	pthread_mutex_lock(&queue->mutex);
	queue->running = 0;
	
	/* Free any remaining buffered data */
	for (int i = 0; i < MAX_BUFFERED_BUNDLES; i++) {
		if (queue->bundles[i].bundleZco && !queue->bundles[i].processed) {
			/* Note: bundleZco cleanup should be handled by ION's ZCO management */
			queue->bundles[i].bundleZco = 0;
		}
	}
	
	pthread_mutex_unlock(&queue->mutex);
	pthread_mutex_destroy(&queue->mutex);
	pthread_cond_destroy(&queue->notEmpty);
	pthread_cond_destroy(&queue->notFull);
}

static int enqueueBundle(BundleQueue *queue, Object bundleZco, BpAncillaryData *ancillaryData, unsigned int bundleLength)
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
	
	BufferedBundle *bundle = &queue->bundles[queue->tail];
	
	/* Store bundle data */
	bundle->bundleZco = bundleZco;
	bundle->ancillaryData = *ancillaryData;
	bundle->bundleLength = bundleLength;
	gettimeofday(&bundle->arrivalTime, NULL);
	bundle->processed = 0;  /* Mark as not processed */
	
	/* Calculate delay for THIS specific bundle */
	bundle->delaySeconds = calculateMarsDelay();
	
	/* Calculate when to send this bundle */
	bundle->sendTime = bundle->arrivalTime;
	long long delayMicroseconds = (long long)(bundle->delaySeconds * 1000000.0);
	bundle->sendTime.tv_usec += delayMicroseconds;
	
	/* Handle overflow */
	while (bundle->sendTime.tv_usec >= 1000000) {
		bundle->sendTime.tv_sec++;
		bundle->sendTime.tv_usec -= 1000000;
	}
	
	queue->tail = (queue->tail + 1) % MAX_BUFFERED_BUNDLES;
	queue->count++;
	
	pthread_cond_signal(&queue->notEmpty);
	pthread_mutex_unlock(&queue->mutex);
	
	return 0;
}

/* Send a single bundle in its own thread */
static void *sendSingleBundle(void *arg)
{
	SenderThreadData *data = (SenderThreadData *)arg;
	BufferedBundle *bundle = data->bundle;
	struct timeval now;
	long long sleepMicroseconds;
	int bytesSent;
	
	/* Calculate how long to sleep until send time */
	gettimeofday(&now, NULL);
	
	sleepMicroseconds = (bundle->sendTime.tv_sec - now.tv_sec) * 1000000LL +
			    (bundle->sendTime.tv_usec - now.tv_usec);
	
	if (sleepMicroseconds > 0) {
		/* Sleep until the exact send time */
		microsnooze((unsigned int)sleepMicroseconds);
	}
	
	/* Check for link loss - randomly drop bundle */
	if (shouldDropBundle()) {
		/* Simulate bundle loss - just drop it without sending */
		bundle->processed = 1;  /* Mark as processed */
		free(data);
		return NULL;
	}
	
	/* Send the bundle */
	bytesSent = sendBundleByUDP(data->socketName, data->ductSocket,
			bundle->bundleLength, bundle->bundleZco, data->buffer);
	
	if (bytesSent < bundle->bundleLength) {
		putErrmsg("Bundle transmission failed in parallel sender.", itoa(bytesSent));
	}
	
	/* Mark as processed */
	bundle->processed = 1;
	free(data);
	
	return NULL;
}

/* Find ready bundles and spawn sender threads for them */
static void *bundleScheduler(void *parm)
{
	OutductThreadParms *otp = (OutductThreadParms *)parm;
	BundleQueue *queue = otp->queue;
	struct timeval now;
	pthread_t senderThread;
	SenderThreadData *threadData;
	
	while (queue->running) {
		pthread_mutex_lock(&queue->mutex);
		
		gettimeofday(&now, NULL);
		
		/* Check all bundles to see if any are ready */
		for (int i = 0; i < MAX_BUFFERED_BUNDLES; i++) {
			BufferedBundle *bundle = &queue->bundles[i];
			
			/* Skip empty slots or already processed bundles */
			if (bundle->bundleZco == 0 || bundle->processed) {
				continue;
			}
			
			/* Check if this bundle is ready to be sent */
			if (now.tv_sec > bundle->sendTime.tv_sec ||
			    (now.tv_sec == bundle->sendTime.tv_sec && 
			     now.tv_usec >= bundle->sendTime.tv_usec)) {
				
				/* Create thread data */
				threadData = malloc(sizeof(SenderThreadData));
				if (threadData == NULL) {
					continue;
				}
				
				threadData->bundle = bundle;
				threadData->socketName = otp->socketName;
				threadData->ductSocket = otp->ductSocket;
				threadData->buffer = otp->buffer;
				
				/* Spawn sender thread for this bundle */
				if (pthread_create(&senderThread, NULL, sendSingleBundle, threadData) == 0) {
					pthread_detach(senderThread);  /* Don't wait for it */
					bundle->processed = 1;  /* Mark as being processed */
				} else {
					free(threadData);
				}
			}
		}
		
		pthread_mutex_unlock(&queue->mutex);
		
		/* Sleep briefly before checking again */
		microsnooze(10000);  /* 10ms */
	}
	
	return NULL;
}

static sm_SemId		udpmarsdelaycloSemaphore(sm_SemId *semid)
{
	static sm_SemId	semaphore = -1;
	
	if (semid)
	{
		semaphore = *semid;
	}

	return semaphore;
}

static void	shutDownClo(int signum)
{
	sm_SemEnd(udpmarsdelaycloSemaphore(NULL));
}

/*	*	*	Main thread functions	*	*	*	*/

static unsigned long	getUsecTimestamp()
{
	struct timeval	tv;

	getCurrentTime(&tv);
	return ((tv.tv_sec * 1000000) + tv.tv_usec);
}

#if defined (ION_LWT)
int	udpmarsdelayclo(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char			*endpointSpec = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char			*endpointSpec = (argc > 1 ? argv[1] : NULL);
#endif
	unsigned short		portNbr;
	unsigned int		hostNbr;
	char			ownHostName[MAXHOSTNAMELEN];
	struct sockaddr		socketName;
	struct sockaddr_in	*inetName;

	unsigned char		*buffer;
	VOutduct		*vduct;
	PsmAddress		vductElt;
	Sdr			sdr;
	Outduct			outduct;
	Object			planDuctList;
	Object			planObj = 0;
	BpPlan			plan;
	IonNeighbor		*neighbor = NULL;
	PsmAddress		nextElt;
	Object			bundleZco;
	BpAncillaryData		ancillaryData;
	unsigned int		bundleLength;
	int			ductSocket = -1;

	/* Parallel processing components */
	BundleQueue		bundleQueue;
	OutductThreadParms	otp;
	pthread_t		schedulerThread;

	/*	Rate control calculation is based on treating elapsed
	 *	time as a currency.					*/

	float			timeCostPerByte;/*	In seconds.	*/
	unsigned long		startTimestamp;	/*	Billing cycle.	*/
	unsigned int		totalPaid;	/*	Since last send.*/
	unsigned int		currentPaid;	/*	Sending seg.	*/
	float			totalCostSecs;	/*	For this seg.	*/
	unsigned int		totalCost;	/*	Microseconds.	*/
	unsigned int		balanceDue;	/*	Until next seg.	*/
	unsigned int		prevPaid = 0;	/*	Prior snooze.	*/

	if (endpointSpec == NULL)
	{
		PUTS("Usage: udpmarsdelayclo <remote node's host name>[:<its port number>]");
		return 0;
	}

	parseSocketSpec(endpointSpec, &portNbr, &hostNbr);
	if (portNbr == 0)
	{
		portNbr = BpUdpDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	if (hostNbr == 0)		/*	Default to local host.	*/
	{
		getNameOfHost(ownHostName, sizeof ownHostName);
		hostNbr = getInternetAddress(ownHostName);
	}

	hostNbr = htonl(hostNbr);
	memset((char *) &socketName, 0, sizeof socketName);
	inetName = (struct sockaddr_in *) &socketName;
	inetName->sin_family = AF_INET;
	inetName->sin_port = portNbr;
	memcpy((char *) &(inetName->sin_addr.s_addr), (char *) &hostNbr, 4);

	/*	Finish validating command-line arguments.		*/

	if (bpAttach() < 0)
	{
		putErrmsg("udpmarsdelayclo can't attach to BP.", NULL);
		return -1;
	}

	/* Initialize random number generator for link loss simulation */
	srand((unsigned int)time(NULL));

	buffer = MTAKE(UDPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("No memory for UDP buffer in udpmarsdelayclo.", NULL);
		return -1;
	}

	findOutduct("udp", endpointSpec, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such udp duct.", endpointSpec);
		MRELEASE(buffer);
		return -1;
	}

	if (vduct->cloPid != ERROR && vduct->cloPid != sm_TaskIdSelf())
	{
		putErrmsg("CLO task is already started for this duct.",
				itoa(vduct->cloPid));
		MRELEASE(buffer);
		return -1;
	}

	/*	All command-line arguments are now validated.		*/

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

	/* Initialize bundle queue */
	initBundleQueue(&bundleQueue);
	otp.vduct = vduct;
	otp.queue = &bundleQueue;
	otp.running = 1;
	otp.socketName = &socketName;
	otp.ductSocket = &ductSocket;
	otp.buffer = buffer;

	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	oK(udpmarsdelaycloSemaphore(&(vduct->semaphore)));
	isignal(SIGTERM, shutDownClo);

	/* Start the scheduler thread */
	if (pthread_begin(&schedulerThread, NULL, bundleScheduler, &otp))
	{
		putSysErrmsg("udpmarsdelayclo can't create scheduler thread", NULL);
		MRELEASE(buffer);
		return -1;
	}

	/*	Can now begin transmitting to remote duct.		*/

	{
		char	memoBuf[1024];
		double	currentDelay = calculateMarsDelay();

		isprintf(memoBuf, sizeof(memoBuf),
				"[i] udpmarsdelayclo is running, spec = '%s', Mars delay = %.2f sec, link loss = %.1f%% (parallel processing)",
				endpointSpec, currentDelay, LINK_LOSS_PERCENTAGE);
		writeMemo(memoBuf);
	}

	startTimestamp = getUsecTimestamp();
	while (!(sm_SemEnded(vduct->semaphore)))
	{
		if (bpDequeue(vduct, &bundleZco, &ancillaryData, 0) < 0)
		{
			putErrmsg("Can't dequeue bundle.", NULL);
			break;
		}

		if (bundleZco == 0)	/*	Outduct closed.		*/
		{
			writeMemo("[i] udpmarsdelayclo outduct closed.");
			sm_SemEnd(udpmarsdelaycloSemaphore(NULL));/*	Stop.	*/
			continue;
		}

		if (bundleZco == 1)	/*	Got a corrupt bundle.	*/
		{
			continue;	/*	Get next bundle.	*/
		}

		CHKZERO(sdr_begin_xn(sdr));
		bundleLength = zco_length(sdr, bundleZco);
		sdr_exit_xn(sdr);

		/* Enqueue bundle for parallel processing - NO BLOCKING HERE */
		if (enqueueBundle(&bundleQueue, bundleZco, &ancillaryData, bundleLength) < 0) {
			putErrmsg("Can't buffer bundle - queue full.", NULL);
			continue;
		}

		/*	Rate control calculation is based on treating
		 *	elapsed time as a currency, the price you
		 *	pay (by microsnooze) for sending a segment
		 *	of a given size.  All cost figures are
		 *	expressed in microseconds except the computed
		 *	totalCostSecs of the segment.			*/

		totalPaid = getUsecTimestamp() - startTimestamp;

		/*	Start clock for next bill.			*/

		startTimestamp = getUsecTimestamp();

		/*	Compute time balance due.			*/

		if (totalPaid >= prevPaid)
		{
		/*	This should always be true provided that
		 *	clock_gettime() is supported by the O/S.	*/

			currentPaid = totalPaid - prevPaid;
		}
		else
		{
			currentPaid = 0;
		}

		/*	Get current time cost, in seconds, per byte.	*/

		if (neighbor == NULL)
		{
			if (planObj && plan.neighborNodeNbr)
			{
				neighbor = findNeighbor(getIonVdb(),
						plan.neighborNodeNbr, &nextElt);
			}
		}

		if (neighbor && neighbor->xmitRate > 0)
		{
			timeCostPerByte = 1.0 / (neighbor->xmitRate);
		}
		else	/*	No link service rate control.		*/ 
		{
			timeCostPerByte = 0.0;
		}

		totalCostSecs = timeCostPerByte * computeECCC(bundleLength);
		totalCost = totalCostSecs * 1000000.0;	/*	usec.	*/
		if (totalCost > currentPaid)
		{
			balanceDue = totalCost - currentPaid;
		}
		else
		{
			balanceDue = 0;
		}

		if (balanceDue > 0)
		{
			microsnooze(balanceDue);
		}

		prevPaid = balanceDue;

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/* Shut down parallel processing */
	otp.running = 0;
	bundleQueue.running = 0;
	pthread_detach(schedulerThread);

	if (ductSocket != -1)
	{
		closesocket(ductSocket);
	}

	destroyBundleQueue(&bundleQueue);
	writeErrmsgMemos();
	writeMemo("[i] udpmarsdelayclo duct has ended.");
	MRELEASE(buffer);
	ionDetach();
	return 0;
}