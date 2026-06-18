/* Stress the signal-vs-poll/read handshake on the pollable event: many reader
 * threads poll() one shared (single-consumer) event and drain it with
 * non-blocking reads while the publisher signals flat out. The invariant that
 * must hold is conservation: no signal is lost and none is counted twice, so
 * the sum of all counts read (plus a final drain) equals the number of signals
 * sent. Pass criteria: that equality, no hang, no crash. Kernel-side WARN/oops
 * checked by the caller via dmesg.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "event.h"

#define READERS 8
#define SECONDS 5

static int evt;
static _Atomic int stop;
static _Atomic long total_read;   /* signals consumed by readers */
static _Atomic long poll_timeouts;

static void *reader(void *arg)
{
	(void)arg;
	while (!atomic_load(&stop)) {
		struct pollfd p = { .fd = evt, .events = POLLIN };
		uint64_t count;
		int r = poll(&p, 1, 1);

		if (r < 0)
			exit(2);
		if (r == 0) {
			atomic_fetch_add(&poll_timeouts, 1);
			continue;
		}
		/* Another reader may have drained it first (EAGAIN): fine. */
		if (event_read(evt, &count) == 0)
			atomic_fetch_add(&total_read, (long)count);
	}
	return NULL;
}

int main(void)
{
	pthread_t t[READERS];
	time_t end = time(NULL) + SECONDS;
	long sigs = 0;
	uint64_t count;
	int i;

	evt = create_event_flags(O_NONBLOCK);
	if (evt < 0)
		return 1;

	for (i = 0; i < READERS; i++)
		pthread_create(&t[i], NULL, reader, NULL);

	while (time(NULL) < end) {
		signal_event(evt);
		sigs++;
		if (sigs % 64 == 0)
			usleep(100); /* let readers actually run */
	}

	atomic_store(&stop, 1);
	for (i = 0; i < READERS; i++)
		pthread_join(t[i], NULL);

	/* Readers are gone; drain whatever is left so the count is exact. */
	if (event_read(evt, &count) == 0)
		atomic_fetch_add(&total_read, (long)count);

	printf("storm: %ld signals, %ld consumed, %ld poll timeouts\n", sigs,
	       atomic_load(&total_read), atomic_load(&poll_timeouts));

	if (atomic_load(&total_read) != sigs)
		return 3; /* a signal was lost or double-counted */

	/* after the storm: a clean signal + read still works */
	signal_event(evt);
	if (event_read(evt, &count) != 0 || count != 1)
		return 4;
	close_event(evt);
	printf("storm test passed\n");
	return 0;
}
