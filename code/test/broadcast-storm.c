/* Stress the broadcast + per-subscription consume path: many subscription
 * threads on one event poll+read flat out while the publisher signals flat
 * out. Every subscription subscribes before the first signal, so the invariant
 * that must hold is conservation per subscriber: each one observes *every*
 * signal exactly once (broadcast, no loss, no double-count), i.e. each
 * subscription's summed read counts == signals sent. Pass criteria: that
 * equality for all readers, no hang, no crash. Kernel-side WARN/oops checked by
 * the caller via dmesg.
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
static _Atomic int subscribed;
static _Atomic int go;
static _Atomic int stop;
static long per_reader[READERS];

static void *reader(void *arg)
{
	long idx = (long)arg;
	uint64_t total = 0, count;
	int sub = subscribe_event(evt);

	if (sub < 0 || subscription_set_nonblock(sub) < 0)
		exit(2);
	atomic_fetch_add(&subscribed, 1);
	while (!atomic_load(&go))
		; /* spin until every reader has subscribed */

	while (!atomic_load(&stop)) {
		struct pollfd p = { .fd = sub, .events = POLLIN };

		if (poll(&p, 1, 1) == 1 && event_read(sub, &count) == 0)
			total += count;
	}
	/* drain whatever is left after the last signal */
	while (event_read(sub, &count) == 0 && count > 0)
		total += count;

	per_reader[idx] = (long)total;
	close_subscription(sub);
	return NULL;
}

int main(void)
{
	pthread_t t[READERS];
	time_t end;
	long sigs = 0;
	int i, ok = 1;

	evt = create_event();
	if (evt < 0)
		return 1;

	for (i = 0; i < READERS; i++)
		pthread_create(&t[i], NULL, reader, (void *)(long)i);

	while (atomic_load(&subscribed) < READERS)
		; /* all readers subscribed before the first signal */
	atomic_store(&go, 1);

	end = time(NULL) + SECONDS;
	while (time(NULL) < end) {
		signal_event(evt);
		sigs++;
		if (sigs % 64 == 0)
			usleep(100); /* let readers run and drain */
	}

	atomic_store(&stop, 1);
	for (i = 0; i < READERS; i++)
		pthread_join(t[i], NULL);

	printf("storm: %ld signals to %d subscribers\n", sigs, READERS);
	for (i = 0; i < READERS; i++) {
		if (per_reader[i] != sigs) {
			printf("FAIL: reader %d saw %ld of %ld signals\n", i,
			       per_reader[i], sigs);
			ok = 0;
		}
	}
	close_event(evt);
	if (!ok)
		return 3;
	printf("storm test passed (every subscriber saw every signal)\n");
	return 0;
}
