/* Stress the v5 membership-vs-signal handshake: each waiter thread churns its
 * registration (add -> wait on a tiny timeout -> remove) on the shared event
 * while the publisher signals flat out. This hammers list insertion and
 * removal under one lock against signal_event() walking the list under the
 * other, exercising the lock order and the listener lifetime. Pass criteria:
 * no hang, no crash, sane counters, and a clean add + signal + wait still
 * works once the storm stops. Kernel-side WARN/oops checked by the caller via
 * dmesg.
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
	unsigned int seed = (unsigned int)(uintptr_t)arg;
	int w = create_waiter();

	if (w < 0)
		exit(2);

	while (!atomic_load(&stop)) {
		int ready[1], r;

		if (waiter_add(w, evt) < 0)
			exit(3);
		r = waiter_wait(w, ready, 1, rand_r(&seed) % 2);
		if (r == 1)
			atomic_fetch_add(&signaled_total, 1);
		else if (r == 0)
			atomic_fetch_add(&timedout_total, 1);
		else
			exit(4);
		if (waiter_remove(w, evt) < 0)
			exit(5);
	}
	close_waiter(w);
	return NULL;
}

int main(void)
{
	pthread_t t[WAITERS];
	time_t end = time(NULL) + SECONDS;
	int ready[1], w;
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
			usleep(100); /* let the tiny timeouts actually expire */
	}

	atomic_store(&stop, 1);
	signal_event(evt);
	for (i = 0; i < WAITERS; i++)
		pthread_join(t[i], NULL);

	printf("storm: %ld signals, %ld signaled returns, %ld timeouts\n", sigs,
	       atomic_load(&signaled_total), atomic_load(&timedout_total));

	/* after the storm: a clean add + signal + wait must still work */
	w = create_waiter();
	if (w < 0 || waiter_add(w, evt) < 0)
		return 2;
	signal_event(evt);
	if (waiter_wait(w, ready, 1, EVT_WAIT_ZERO) != 1 || ready[0] != evt)
		return 3; /* a fresh signal must be reported at once */
	if (waiter_wait(w, ready, 1, 100) != 0)
		return 4; /* and then nothing until the next signal */
	close_waiter(w);
	close_event(evt);
	printf("storm test passed\n");
	return 0;
}
