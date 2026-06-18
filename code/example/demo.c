// SPDX-License-Identifier: MIT
/*
 * demo.c - wait on many events at once with epoll.
 *
 * The program:
 *   1. creates N events,
 *   2. adds them all to one epoll instance,
 *   3. forks a signaler child that, after a beat, signals every other event
 *      (and one of them twice, to show coalescing),
 *   4. blocks in epoll_wait and reports each event as it becomes ready,
 *      reading the signal count off the fd.
 *
 * This is the point of the object: it is a first-class pollable fd, so the
 * kernel's own epoll/poll/select do the waiting. No custom wait mechanism.
 *
 * Build:  make           (see Makefile, pulls in ../lib/event.h)
 * Run:    ./demo [num_events]      (default 5; requires event.ko loaded)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "event.h"

#define DEFAULT_EVENTS 5

/* Map an event fd back to its index for tidy printing. */
static int index_of(const int *evts, int n, int fd)
{
	int i;

	for (i = 0; i < n; i++)
		if (evts[i] == fd)
			return i;
	return -1;
}

int main(int argc, char **argv)
{
	int n = DEFAULT_EVENTS;
	int ep, i, expected = 0, seen = 0, failures = 0;
	int *evts;
	pid_t kid;

	if (argc > 1) {
		n = atoi(argv[1]);
		if (n < 1) {
			fprintf(stderr, "num_events must be >= 1\n");
			return 2;
		}
	}

	evts = calloc(n, sizeof(*evts));
	if (!evts) {
		perror("calloc");
		return 1;
	}

	ep = epoll_create1(0);
	if (ep < 0) {
		perror("epoll_create1");
		return 1;
	}

	for (i = 0; i < n; i++) {
		struct epoll_event ev = { .events = EPOLLIN };

		evts[i] = create_event();
		if (evts[i] < 0) {
			fprintf(stderr,
				"create_event failed: %s\n"
				"(is event.ko loaded? `sudo insmod ../module/event.ko`)\n",
				strerror(errno));
			return 1;
		}
		ev.data.fd = evts[i];
		if (epoll_ctl(ep, EPOLL_CTL_ADD, evts[i], &ev) < 0) {
			perror("epoll_ctl");
			return 1;
		}
		if (i % 2 == 0)
			expected++; /* the child will signal even indices */
	}
	printf("[main | pid %d] created %d events, watching them with epoll\n",
	       (int)getpid(), n);
	fflush(stdout);

	kid = fork();
	if (kid == 0) {
		close(ep);
		sleep(1); /* let the parent reach epoll_wait first */
		printf("[signaler | pid %d] signaling every other event\n",
		       (int)getpid());
		fflush(stdout);
		for (i = 0; i < n; i += 2)
			signal_event(evts[i]);
		signal_event(evts[0]); /* twice: coalesces into one wake */
		_exit(0);
	}

	/* Block until every event we expect has reported in. */
	while (seen < expected) {
		struct epoll_event out[64];
		int k = epoll_wait(ep, out, 64, 5000);

		if (k < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			failures++;
			break;
		}
		if (k == 0) {
			fprintf(stderr, "[main] timed out waiting for events\n");
			failures++;
			break;
		}
		for (i = 0; i < k; i++) {
			int fd = out[i].data.fd;
			uint64_t count = 0;

			if (event_read(fd, &count) < 0) {
				perror("event_read");
				failures++;
				continue;
			}
			printf("  [main] event %d ready (%llu signal%s)\n",
			       index_of(evts, n, fd), (unsigned long long)count,
			       count == 1 ? "" : "s");
			seen++;
		}
	}

	waitpid(kid, NULL, 0);
	for (i = 0; i < n; i++)
		close_event(evts[i]);
	close(ep);
	free(evts);

	printf("[main | pid %d] done (%d failure%s)\n", (int)getpid(), failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
