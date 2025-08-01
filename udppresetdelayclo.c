/*
	udppresetdelayclo.c:	UDP Preset Delay convergence-layer output daemon
				with timed bundle processing and link loss simulation.

	Based on original ION UDP convergence layer (udpclo.c)
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
	Object bundleZco;
	BpAncillaryData ancillaryData;
	unsigned int bundleLength;
	struct timeval arrivalTime;
	struct timeval sendTime;     /* When to send this bundle */
	int processed;               /* Flag to mark as processed */
} TimedBundle;

typedef struct {
	TimedBundle bundles[MAX_TIMED_BUNDLES];
	int count;
	pthread_mutex_t mutex;
	int running;
} TimedBundleQueue;

static TimedBundleQueue timedQueue;

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

/* Initialize timed queue */
static void initTimedQueue(void)
{
	memset(&timedQueue, 0, sizeof(TimedBundleQueue));
	pthread_mutex_init(&timedQueue.mutex, NULL);
	timedQueue.running = 1;
}

/* Add bundle to timed queue */
static int addTimedBundle(Object bundleZco, BpAncillaryData *ancillaryData, unsigned int bundleLength)
{
	pthread_mutex_lock(&timedQueue.mutex);
	
	if (timedQueue.count >= MAX_TIMED_BUNDLES) {
		pthread_mutex_unlock(&timedQueue.mutex);
		return -1;  /* Queue full */
	}
	
	TimedBundle *bundle = &timedQueue.bundles[timedQueue.count];
	bundle->bundleZco = bundleZco;
	bundle->ancillaryData = *ancillaryData;
	bundle->bundleLength = bundleLength;
	gettimeofday(&bundle->arrivalTime, NULL);
	bundle->processed = 0;
	
	/* Calculate send time = arrival time + delay with better precision */
	bundle->sendTime = bundle->arrivalTime;
	double delaySeconds = getPresetDelay();
	long delaySecondsWhole = (long)delaySeconds;
	long delayMicroseconds = (long)((delaySeconds - delaySecondsWhole) * 1000000.0);
	
	bundle->sendTime.tv_sec += delaySecondsWhole;
	bundle->sendTime.tv_usec += delayMicroseconds;
	
	/* Handle overflow */
	if (bundle->sendTime.tv_usec >= 1000000) {
		bundle->sendTime.tv_sec++;
		bundle->sendTime.tv_usec -= 1000000;
	}
	
	timedQueue.count++;
	pthread_mutex_unlock(&timedQueue.mutex);
	return 0;
}

/* Process ready bundles (send those whose time has come) */
static void processReadyBundles(struct sockaddr *socketName, int *ductSocket, unsigned char *buffer, Sdr sdr, IonNeighbor *neighbor, float *timeCostPerByte, Object planObj, BpPlan *plan)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	
	/* Update neighbor if needed */
	if (neighbor == NULL && planObj && plan->neighborNodeNbr) {
		PsmAddress nextElt;
		neighbor = findNeighbor(getIonVdb(), plan->neighborNodeNbr, &nextElt);
	}
	
	pthread_mutex_lock(&timedQueue.mutex);
	
	for (int i = 0; i < timedQueue.count; i++) {
		TimedBundle *bundle = &timedQueue.bundles[i];
		
		if (bundle->processed) continue;
		
		/* Check if this bundle is ready to be sent */
		if (now.tv_sec > bundle->sendTime.tv_sec ||
		    (now.tv_sec == bundle->sendTime.tv_sec && 
		     now.tv_usec >= bundle->sendTime.tv_usec)) {
			
			/* Check for link loss */
			if (shouldDropBundle()) {
				/* Simulate bundle loss - just mark as processed */
				bundle->processed = 1;
				continue;
			}
			
			/* Use the pre-calculated bundle length to avoid SDR conflicts */
			unsigned int actualLength = bundle->bundleLength;
			
			/* Send the bundle */
			int bytesSent = sendBundleByUDP(socketName, ductSocket,
					actualLength, bundle->bundleZco, buffer);
			
			if (bytesSent < actualLength) {
				putErrmsg("Bundle transmission failed in timed sender.", itoa(bytesSent));
			} else {
				/* Apply rate control only for successfully sent bundles */
				if (neighbor && neighbor->xmitRate > 0) {
					*timeCostPerByte = 1.0 / (neighbor->xmitRate);
					float totalCostSecs = (*timeCostPerByte) * computeECCC(actualLength);
					unsigned int totalCost = totalCostSecs * 1000000.0;  /* usec */
					if (totalCost > 0) {
						microsnooze(totalCost);
					}
				}
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
	timedQueue.count = 0;
	pthread_mutex_unlock(&timedQueue.mutex);
	pthread_mutex_destroy(&timedQueue.mutex);
}

static sm_SemId		udppresetdelaycloSemaphore(sm_SemId *semid)
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
	sm_SemEnd(udppresetdelaycloSemaphore(NULL));
}

/*	*	*	Main thread functions	*	*	*	*/


#if defined (ION_LWT)
int	udppresetdelayclo(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
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
	Object			bundleZco;
	BpAncillaryData		ancillaryData;
	unsigned int		bundleLength;
	int			ductSocket = -1;
	float			timeCostPerByte = 0.0;

	if (endpointSpec == NULL)
	{
		PUTS("Usage: udppresetdelayclo <remote node's host name>[:<its port number>]");
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
	if (bpAttach() < 0)
	{
		putErrmsg("udppresetdelayclo can't attach to BP.", NULL);
		return -1;
	}

	buffer = MTAKE(UDPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("No memory for UDP buffer in udppresetdelayclo.", NULL);
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

	/* Initialize random number generator for link loss simulation */
	srand((unsigned int)time(NULL));
	
	/* Initialize timed queue */
	initTimedQueue();

	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	oK(udppresetdelaycloSemaphore(&(vduct->semaphore)));
	isignal(SIGTERM, shutDownClo);

	/*	Can now begin transmitting to remote duct.		*/

	{
		char	memoBuf[1024];
		double	currentDelay = getPresetDelay();

		isprintf(memoBuf, sizeof(memoBuf),
				"[i] udppresetdelayclo is running, spec = '%s', preset delay = %.1f sec, link loss = %.1f%% (timed processing)",
				endpointSpec, currentDelay, LINK_LOSS_PERCENTAGE);
		writeMemo(memoBuf);
	}

	/* Initialize for bundle processing loop */
	while (!(sm_SemEnded(vduct->semaphore)))
	{
		/* Check if ION is shutting down before attempting operations */
		if (getIonsdr() == NULL) {
			writeMemo("[i] udppresetdelayclo ION shutting down.");
			break;
		}
		
		if (bpDequeue(vduct, &bundleZco, &ancillaryData, 0) < 0)
		{
			putErrmsg("Can't dequeue bundle.", NULL);
			break;
		}

		if (bundleZco == 0)	/*	Outduct closed.		*/
		{
			writeMemo("[i] udppresetdelayclo outduct closed.");
			sm_SemEnd(udppresetdelaycloSemaphore(NULL));/*	Stop.	*/
			continue;
		}

		if (bundleZco == 1)	/*	Got a corrupt bundle.	*/
		{
			continue;	/*	Get next bundle.	*/
		}

		/* Check SDR availability before transaction */
		if (getIonsdr() == NULL) {
			writeMemo("[i] udppresetdelayclo SDR unavailable during shutdown.");
			break;
		}
		
		if (sdr_begin_xn(sdr) < 0) {
			putErrmsg("Can't begin SDR transaction.", NULL);
			continue;
		}
		bundleLength = zco_length(sdr, bundleZco);
		sdr_exit_xn(sdr);

		/* Add bundle to timed queue for delayed sending */
		if (addTimedBundle(bundleZco, &ancillaryData, bundleLength) < 0) {
			putErrmsg("Can't queue timed bundle - queue full.", NULL);
			continue;
		}
		
		/* Process any ready bundles (non-blocking) */
		processReadyBundles(&socketName, &ductSocket, buffer, sdr, neighbor, &timeCostPerByte, planObj, &plan);

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/* Process any remaining bundles before shutdown */
	processReadyBundles(&socketName, &ductSocket, buffer, sdr, neighbor, &timeCostPerByte, planObj, &plan);
	
	if (ductSocket != -1)
	{
		closesocket(ductSocket);
	}

	destroyTimedQueue();
	writeErrmsgMemos();
	writeMemo("[i] udppresetdelayclo duct has ended.");
	MRELEASE(buffer);
	ionDetach();
	return 0;
}