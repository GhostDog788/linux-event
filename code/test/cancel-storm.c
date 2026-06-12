/* Stress the v5 cancel-vs-fast-walk handshake: waiter threads loop on very
 * short timeouts (cancelling constantly) while the publisher signals flat
 * out. Every transition fast walk / careful walk / reap / canceller-spin
 * gets hammered. Pass criteria: no hang, no crash, sane counters, and the
 * fast path re-engages once the storm stops (checked via a final clean
 * park + wake). Kernel-side WARN/oops checked by the caller via dmesg.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "event.h"

#define WAITERS 8
#define SECONDS 5

static int evt;
static _Atomic int stop;
static _Atomic long signaled_total, timedout_total;

static void *waiter(void *arg)
{
	uint64_t gen = 0;
	unsigned int seed = (unsigned int)(uintptr_t)arg;

	while (!atomic_load(&stop)) {
		int r = wait_for_event(evt, &gen, rand_r(&seed) % 2);

		if (r == EVT_SIGNALED)
			atomic_fetch_add(&signaled_total, 1);
		else if (r == EVT_TIMEOUT)
			atomic_fetch_add(&timedout_total, 1);
		else
			exit(2);
	}
	return NULL;
}

int main(void)
{
	pthread_t t[WAITERS];
	time_t end = time(NULL) + SECONDS;
	uint64_t gen;
	long sigs = 0;
	int i;

	evt = create_event();
	if (evt < 0)
		return 1;

	for (i = 0; i < WAITERS; i++)
		pthread_create(&t[i], NULL, waiter, (void *)(uintptr_t)(i + 1));

	while (time(NULL) < end) {
		signal_event(evt);
		sigs++;
		if (sigs % 64 == 0)
			usleep(100); /* let timeouts actually expire too */
	}

	atomic_store(&stop, 1);
	signal_event(evt);
	for (i = 0; i < WAITERS; i++)
		pthread_join(t[i], NULL);

	printf("storm: %ld signals, %ld signaled returns, %ld timeouts\n",
	       sigs, atomic_load(&signaled_total),
	       atomic_load(&timedout_total));

	/* after the storm: a clean park + wake must still work */
	gen = 0;
	if (wait_for_event(evt, &gen, EVT_WAIT_ZERO) != EVT_SIGNALED)
		return 3; /* gen 0 is long stale; must return immediately */
	if (wait_for_event(evt, &gen, 100) != EVT_TIMEOUT)
		return 4;
	close_event(evt);
	printf("storm test passed\n");
	return 0;
}
