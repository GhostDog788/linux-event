// SPDX-License-Identifier: MIT
/*
 * demo.c - broadcast one signal to many listeners, then a k-of-n wait.
 *
 * Phase 1 (broadcast): the parent creates one event and forks N listeners that
 * each SUBSCRIBE the inherited event and block; one signal wakes all N.
 * Phase 2 (wait-first): one process subscribes to several events and waits for
 * the first k of them, via the kernel's poll.
 *
 * Build:  make           (see Makefile, pulls in ../lib/event.h)
 * Run:    ./demo [num_listeners]      (default 5; requires event.ko loaded)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "event.h"

#define DEFAULT_LISTENERS 5

/* Child: subscribe to the shared event, block, report the wake. */
static int run_listener(int evt, int id)
{
	int sub = subscribe_event(evt);

	if (sub < 0) {
		fprintf(stderr, "  [listener %d | pid %d] subscribe failed: %s\n",
			id, (int)getpid(), strerror(errno));
		return 1;
	}
	printf("  [listener %d | pid %d] subscribed, waiting...\n", id,
	       (int)getpid());
	fflush(stdout);

	if (event_wait(sub, 5000) < 1) {
		fprintf(stderr, "  [listener %d | pid %d] never woke\n", id,
			(int)getpid());
		return 1;
	}
	printf("  [listener %d | pid %d] >>> woke up, event received!\n", id,
	       (int)getpid());
	fflush(stdout);
	close_subscription(sub);
	return 0;
}

static int phase_broadcast(int listeners)
{
	int evt, i, failures = 0;
	pid_t *kids;

	evt = create_event();
	if (evt < 0) {
		fprintf(stderr,
			"create_event failed: %s\n"
			"(is event.ko loaded? `sudo insmod ../module/event.ko`)\n",
			strerror(errno));
		return 1;
	}
	printf("[publisher | pid %d] created event, forking %d listeners\n",
	       (int)getpid(), listeners);
	fflush(stdout);

	kids = calloc(listeners, sizeof(*kids));
	if (!kids) {
		perror("calloc");
		close_event(evt);
		return 1;
	}

	for (i = 0; i < listeners; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			failures++;
			kids[i] = -1;
			continue;
		}
		if (pid == 0) {
			int rc = run_listener(evt, i);

			close_event(evt);
			_exit(rc);
		}
		kids[i] = pid;
	}

	sleep(1); /* let the listeners subscribe and block first */
	printf("[publisher | pid %d] one signal, waking all listeners\n",
	       (int)getpid());
	if (signal_event(evt) < 0) {
		perror("signal_event");
		failures++;
	}

	for (i = 0; i < listeners; i++) {
		int status;

		if (kids[i] < 0)
			continue;
		if (waitpid(kids[i], &status, 0) < 0 || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0)
			failures++;
	}

	close_event(evt);
	free(kids);
	return failures;
}

static int phase_wait_first(void)
{
	int evts[3], subs[3], ready[3], i, n;

	printf("[first] subscribing to 3 events, waiting for the first 2\n");
	for (i = 0; i < 3; i++) {
		evts[i] = create_event();
		subs[i] = evts[i] < 0 ? -1 : subscribe_event(evts[i]);
		if (subs[i] < 0) {
			perror("create/subscribe");
			return 1;
		}
	}

	signal_event(evts[0]);
	signal_event(evts[2]);

	n = event_wait_first(subs, 3, 2, 5000, ready);
	if (n < 2) {
		fprintf(stderr, "[first] expected 2 ready, got %d\n", n);
		return 1;
	}
	printf("[first] %d ready:", n);
	for (i = 0; i < n; i++) {
		int which = ready[i] == subs[0] ? 0 : ready[i] == subs[1] ? 1 : 2;

		printf(" event%d", which);
	}
	printf("\n");

	for (i = 0; i < 3; i++) {
		close_subscription(subs[i]);
		close_event(evts[i]);
	}
	return 0;
}

int main(int argc, char **argv)
{
	int listeners = DEFAULT_LISTENERS, failures = 0;

	if (argc > 1) {
		listeners = atoi(argv[1]);
		if (listeners < 1) {
			fprintf(stderr, "num_listeners must be >= 1\n");
			return 2;
		}
	}

	failures += phase_broadcast(listeners);
	printf("\n");
	failures += phase_wait_first();

	printf("\n[done] %d failure%s\n", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
