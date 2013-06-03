#ifndef PING_H
#define PING_H

#include <netinet/in.h>
#include <sys/time.h>

struct netmask {
	in_addr_t net, mask;
};

int parse_netmask(struct netmask *, const char *);

int ping_open();
int ping_send(int icmp, uint16_t id, uint16_t seq, struct in_addr host);
int ping_recv(int icmp, uint16_t *id, uint16_t *seq, struct in_addr *host, struct timeval *ts);

#endif
