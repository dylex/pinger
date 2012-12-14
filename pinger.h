#ifndef _PINGER_H
#define _PINGER_H

#include <sys/un.h>
#include <netinet/in.h>
#include <stdint.h>

#define PINGER_SOCKET	"/tmp/.pinger"
#define MAX_PING_TIMEOUT	60000000 /* 1 minute */

struct ping {
	in_addr_t host;
	int32_t time; /* us, -errno */
};

#endif
