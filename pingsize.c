#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ping.h"

static bool Stop = false;

static void stop(int sig) 
{
	Stop = true;
}

int main(int argc, char **argv) {
#define DIE(MSG...) ({ fprintf(stderr, MSG); return 1; })

	struct in_addr a;
	if (argc != 2 || !inet_aton(argv[1], &a))
		DIE("Usage: %s IP\n", argv[0]);

	int p = ping_open();
	if (p < 0)
		DIE("ping_open: %m\n");

	if (setuid(getuid()))
		DIE("setuit: %m\n");

	if (signal(SIGTERM, &stop) == SIG_ERR ||
			signal(SIGINT, &stop) == SIG_ERR)
		DIE("signal: %m\n");

	struct pollfd polls[1] = { { p, POLLIN } };

#define RANGE (PING_MAX_SIZE-PING_MIN_SIZE)
	unsigned sent[RANGE] = {}, recvd[RANGE] = {};
	uint16_t last[RANGE] = {};

	srand(time(NULL));
	uint16_t base = rand();
	unsigned total = 0;
	while (!Stop) {
		unsigned l = rand() % RANGE;
		uint16_t id = -1;
		if (ping_send(p, base+l, ++sent[l], PING_MIN_SIZE+l, a))
			DIE("ping_send: %m\n");
		// printf("send     %u %u\n", l, sent[l]);
		fprintf(stderr, "\r%u", ++total);
		do {
			int r = poll(polls, 1, 100);
			if (r < 0) {
				if (errno == EINTR)
					break;
				DIE("poll: %m\n");
			}
			if (!r)
				break;
			uint16_t seq;
			struct in_addr h;
			r = ping_recv(p, &id, &seq, &h, NULL);
			if (r < 0)
				DIE("ping_send: %m\n");
			if (!r)
				continue;
			id -= base;
			if (memcmp(&a, &h, sizeof(struct in_addr)) || id >= RANGE || seq != (uint16_t)sent[id] || seq <= last[id]) {
				fprintf(stderr, "bogey: %s %u %u\n", inet_ntoa(h), id, seq);
				continue;
			}
			// printf("recv %s %u %u\n", id == l ? "   " : "ooo", id, seq);
			recvd[id] ++;
			last[id] = seq;
		} while (id != l);
	}

	for (unsigned l = 0; l < RANGE; l ++)
		if (sent[l])
			printf("%u %u %u\n", PING_MIN_SIZE+l, sent[l], recvd[l]);
}
