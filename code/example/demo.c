// SPDX-License-Identifier: MIT
/*
 * demo.c - demonstrate the event object with many listeners and one sender.
 *
 * The program:
 *   1. creates a single event in the parent,
 *   2. clones itself with fork() into N listener children that all
 *      wait_for_event() on the *inherited* fd (same kernel object),
 *   3. acts as the lone sender in the parent: after the listeners have
 *      registered, it signal_event()s once, waking them all at the same time.
 *
 * Everyone prints to the screen so you can watch the listeners block and then
 * wake together.
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

/* Child path: register on the event, block, then report the wake-up. */
static int run_listener(int evt, int id)
{
	uint64_t gen = 0; /* fresh listener: has seen no signals yet */

	printf("  [listener %d | pid %d] waiting for the event...\n", id,
	       (int)getpid());
	fflush(stdout);

	if (wait_for_event(evt, &gen, EVT_WAIT_FOREVER) != EVT_SIGNALED) {
		fprintf(stderr,
			"  [listener %d | pid %d] wait_for_event failed: %s\n",
			id, (int)getpid(), strerror(errno));
		return 1;
	}

	printf("  [listener %d | pid %d] >>> woke up, event received!\n", id,
	       (int)getpid());
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	int listeners = DEFAULT_LISTENERS;
	int evt, i, failures = 0;
	pid_t *kids;

	if (argc > 1) {
		listeners = atoi(argv[1]);
		if (listeners < 1) {
			fprintf(stderr, "num_listeners must be >= 1\n");
			return 2;
		}
	}

	evt = create_event();
	if (evt < 0) {
		fprintf(stderr,
			"create_event failed: %s\n"
			"(is event.ko loaded? `sudo insmod ../module/event.ko`)\n",
			strerror(errno));
		return 1;
	}
	printf("[sender | pid %d] created event (fd %d), spawning %d listeners\n",
	       (int)getpid(), evt, listeners);
	/* Flush before fork() so children don't inherit (and re-emit) our
	 * still-buffered stdout when output is a pipe rather than a tty. */
	fflush(stdout);

	kids = calloc(listeners, sizeof(*kids));
	if (!kids) {
		perror("calloc");
		close_event(evt);
		return 1;
	}

	/* Clone ourselves into N listeners; each inherits the same event fd. */
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

	/*
	 * Give the listeners a moment to print their "waiting..." line so the
	 * output reads in order. Purely cosmetic: signals are counted in the
	 * event's generation, so a listener that calls wait_for_event() only
	 * after we signal still returns immediately instead of missing it.
	 */
	sleep(1);

	printf("[sender | pid %d] signaling the event, waking all listeners\n",
	       (int)getpid());
	if (signal_event(evt) < 0) {
		fprintf(stderr, "signal_event failed: %s\n", strerror(errno));
		failures++;
	}

	/* Reap the listeners and collect their exit status. */
	for (i = 0; i < listeners; i++) {
		int status;

		if (kids[i] < 0)
			continue;
		if (waitpid(kids[i], &status, 0) < 0) {
			perror("waitpid");
			failures++;
			continue;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			failures++;
	}

	close_event(evt);
	free(kids);

	printf("[sender | pid %d] done (%d failure%s)\n", (int)getpid(),
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
