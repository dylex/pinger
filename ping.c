#include <errno.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include "ping.h"

static int parse_net(in_addr_t *n, const char **s)
{
	const char *p = *s;
	uint32_t x = 0;
	unsigned o;
	for (o = 0;; o ++)
	{
		unsigned long y = strtoul(p, (char **)&p, 0);
		if (y > 255)
			return -1;
		x |= y << (8*(3-o));
		if (o == 4 || *p != '.')
			break;
		p ++;
	}
	*s = p;
	*n = htonl(x);
	return o;
}

int parse_netmask(struct netmask *nm, const char *s)
{
	if (parse_net(&nm->net, &s) < 0)
		return -1;
	if (!*s)
	{
		nm->mask = ~0;
		return 0;
	}
	if (*s++ != '/')
		return -1;
	int r;
	if ((r = parse_net(&nm->mask, &s)) < 0)
		return -1;
	if (*s)
		return -1;
	if (!r && nm->mask && (r = ntohl(nm->mask) >> 24) <= 32)
		nm->mask = ~htonl((1U << (32 - r)) - 1);
	nm->net &= nm->mask;
	return 1;
}

int ping_open()
{
	int s;
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0)
		return -1;
	int opt = 1;
	setsockopt(s, SOL_SOCKET, SO_TIMESTAMP, &opt, sizeof(opt));
	return s;
}

static uint16_t icmp_checksum(struct icmp *i, size_t len)
{
	uint32_t sum = 0;
	uint16_t *p = (uint16_t *)i;
	while (len >= 2)
	{
		sum += *(p++);
		len -= 2;
	}
	if (len)
	{
		uint16_t x = 0;
		*(char *)&x = *(char *)p;
		sum += x;
	}

	sum = (sum & 0xFFFF) + (sum >> 16);
	sum = (sum & 0xFFFF) + (sum >> 16);
	return ~sum;
}

int ping_send(int icmp, uint16_t id, uint16_t seq, struct in_addr host)
{
	struct icmp i = 
		{ .icmp_type = ICMP_ECHO
		, .icmp_id = id
		, .icmp_seq = seq
		};
	i.icmp_cksum = icmp_checksum(&i, sizeof(i));
	struct sockaddr_in a = { AF_INET, 0, host };
	ssize_t r = sendto(icmp, &i, sizeof(i), 0, &a, sizeof(a));
	if (r < 0)
		return -1;
	if (r < sizeof(i))
	{
		errno = ENOBUFS;
		return -1;
	}
	return 0;
}

int ping_recv(int icmp, uint16_t *id, uint16_t *seq, struct in_addr *host, struct timeval *ts)
{
	struct sockaddr_in sa;
	union {
		char buf[4096];
		struct ip ip;
	} buf;
	struct iovec io =
		{ .iov_base = &buf
		, .iov_len = sizeof(buf)
		};
	char ctlbuf[1024];
	struct msghdr msg =
		{ .msg_name = &sa
		, .msg_namelen = sizeof(sa)
		, .msg_iov = &io
		, .msg_iovlen = 1
		, .msg_control = &ctlbuf
		, .msg_controllen = sizeof(ctlbuf)
		};
	ssize_t r = recvmsg(icmp, &msg, 0);
	if (r < 0)
		return -1;
	if (r == 0)
	{
		errno = ECANCELED;
		return -1;
	}
	if (r < sizeof(struct ip) || (r -= buf.ip.ip_hl << 2) < sizeof(struct icmp))
		return 0;
	struct icmp *i = (struct icmp *)(buf.buf + (buf.ip.ip_hl << 2));
	if (i->icmp_type != ICMP_ECHOREPLY || icmp_checksum(i, r) || sa.sin_family != AF_INET)
		return 0;
	*id = i->icmp_id;
	*seq = i->icmp_seq;
	*host = sa.sin_addr;
	struct cmsghdr *cmsg;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMP)
		{
			memcpy(ts, CMSG_DATA(cmsg), sizeof(*ts));
			break;
		}
	return 1;
}
