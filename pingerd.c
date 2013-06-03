#include <unistd.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <argp.h>
#include <grp.h>
#include "pinger.h"
#include "ping.h"

static int Server = -1;
static int Icmp = -1;
static struct sockaddr_un Server_addr = { AF_UNIX, PINGER_SOCKET };
static bool Socket_created;
static const char *Group;
static unsigned Rate = 60, Rate_period = 60; /* 60/minute */

#define MAX_FILTERS	16
enum filter_type {
	FILTER_ACCEPT = 0,
	FILTER_REJECT,
	FILTER_TYPES
};
static struct filter {
	unsigned count;
	struct netmask filters[MAX_FILTERS];
} Filter[FILTER_TYPES];

static struct pinger {
	struct sockaddr_un client;
	socklen_t client_len;
	struct ping req;
	struct timeval sent;
	uint16_t id;
	uint16_t seq;
	uint32_t timeout;
	struct pinger *next, **prev;
} *Pings;
static uint16_t Seq;

static void stop(int sig) __attribute__((noreturn));
static void stop(int sig) 
{
	if (Socket_created)
		unlink(Server_addr.sun_path);
	exit(sig == 0);
}

static void die(const char *msg, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	vfprintf(stderr, msg, args);
	va_end(args);
	stop(0);
}

static void open_server()
{
	if ((Server = socket(PF_UNIX, SOCK_DGRAM, 0)) < 0)
		die("server socket: %m\n");
	if (fcntl(Server, F_SETFL, O_NONBLOCK) < 0)
		die("server fcntl O_NONBLOCK: %m\n");
	if (unlink(Server_addr.sun_path) == -1 && errno != ENOENT)
		die("unlink %s: %m\n", Server_addr.sun_path);
	mode_t omask = umask(0177);
	if (bind(Server, &Server_addr, SUN_LEN(&Server_addr)) < 0)
		die("server bind: %m\n");
	Socket_created = true;
	umask(omask);
	struct group *gr;
	if (Group && (gr = getgrnam(Group)))
		if (chown(Server_addr.sun_path, -1, gr->gr_gid) == 0)
			chmod(Server_addr.sun_path, 0660);
}

static inline bool test_netmask(const struct netmask nm, in_addr_t ip)
{
	return (ip & nm.mask) == nm.net;
}

static bool test_filters(in_addr_t ip)
{
	unsigned i;
	for (i = 0; i < Filter[FILTER_ACCEPT].count; i ++)
		if (test_netmask(Filter[FILTER_ACCEPT].filters[i], ip))
			break;
	if (i && i == Filter[FILTER_ACCEPT].count)
		return false;
	for (i = 0; i < Filter[FILTER_REJECT].count; i ++)
		if (test_netmask(Filter[FILTER_REJECT].filters[i], ip))
			return false;
	return true;
}

static bool test_rate(const struct timeval *t)
{
	static unsigned count;
	static struct timeval s;
	if (t->tv_sec - s.tv_sec > Rate_period)
	{
		count = 0;
		s = *t;
	}
	return ++count <= Rate;
}

static int32_t timeval_diff(const struct timeval *a, const struct timeval *b)
{
	/* assumes no wrap-around, which should be safe with timeouts limited */
	return 1000000 * (a->tv_sec - b->tv_sec) + (a->tv_usec - b->tv_usec);
}

static int pinger_send(struct pinger *p)
{
	gettimeofday(&p->sent, NULL);
	return ping_send(Icmp, p->id, p->seq, (struct in_addr){ p->req.host });
}

static void ping_res(struct pinger *p, int time)
{
	struct ping res = { p->req.host, time };
	sendto(Server, &res, sizeof(res), 0, &p->client, p->client_len);
	if (p->prev && (*p->prev = p->next))
	{
		p->next->timeout += p->timeout;
		p->next->prev = p->prev;
	}
	free(p);
}

static void ping_req(const struct timeval *t)
{
	struct pinger *p = calloc(sizeof(struct pinger), 1);
	if (!p)
		die("malloc(ping): %m\n");
	p->client_len = sizeof(p->client);
	ssize_t r = recvfrom(Server, &p->req, sizeof(p->req), 0, &p->client, &p->client_len);
	if (r < 0)
		die("server recvfrom: %m\n");
	if (r != sizeof(p->req))
		return free(p); /*ping_res(p, -EBADMSG)*/

	if ((p->timeout = (uint32_t)p->req.time) > MAX_PING_TIMEOUT)
		return ping_res(p, -EINVAL);
	if (!test_filters(p->req.host))
		return ping_res(p, -EACCES);
	if (!test_rate(t))
		return ping_res(p, -ENFILE);
	p->id = rand();
	p->seq = htons(Seq++);
	if (pinger_send(p) < 0)
		return ping_res(p, -errno);

	struct pinger **pp = &Pings;
	while ((*pp) && (*pp)->timeout < p->timeout)
	{
		p->timeout -= (*pp)->timeout;
		pp = &(*pp)->next;
	}
	if ((p->next = *pp))
		(*pp)->prev = &p->next;
	p->prev = pp;
	*pp = p;
}

static void pinger_recv(const struct timeval *t)
{
	struct timeval pt = *t;
	uint16_t id, seq;
	struct in_addr host;
	int r = ping_recv(Icmp, &id, &seq, &host, &pt); 
	if (r < 0)
		die("ping recv: %m\n");
	if (!r)
		return;

	struct pinger *p;
	for (p = Pings; p; p = p->next)
		if (id == p->id && seq == p->seq 
				&& host.s_addr == p->req.host)
			break;
	if (!p)
		return;
	/* currently packets failing these checks are ignored above */
	if (seq != p->seq)
	{
		fprintf(stderr, "icmp out of order response: %hu/%hu\n", ntohs(seq), p->seq);
		return;
	}
	if (host.s_addr != p->req.host)
		fprintf(stderr, "icmp response from different IP: %s\n", inet_ntoa(host));
	return ping_res(p, timeval_diff(&pt, &p->sent));
}

static void loop()
{
	struct pollfd polls[2] = 
		{ { .fd = Icmp, .events = POLLIN }
		, { .fd = Server, .events = POLLIN }
		};
	int r = poll(polls, 2, Pings ? (Pings->timeout+999)/1000 : -1);
	if (r < 0)
		die("poll: %m\n");
	if (r == 0)
		return ping_res(Pings, -ETIMEDOUT);
	struct timeval t;
	gettimeofday(&t, NULL);
	if (Pings)
	{
		uint32_t td = timeval_diff(&t, &Pings->sent);
		Pings->timeout = Pings->req.time >= td ? Pings->req.time - td : 0;
	}
	if (polls[0].revents)
		pinger_recv(&t);
	if (polls[1].revents)
		ping_req(&t);
}

static const struct argp_option Options[] = 
	{ { "socket", 'P', "PATH", 0, "listen on socket PATH for ping commands" }
	, { "group", 'g', "NAME", 0, "allow access from group NAME" }
	, { "rate", 'l', "COUNT/PERIOD", 0, "limit to COUNT pings per PERIOD [60/m]" }
	, { "accept", 'a', "IP[/MASK]", 0, "allow pings to given network [all]" }
	, { "reject", 'r', "IP[/MASK]", 0, "reject pings to given network [none]" }
	, { }
	};

static error_t parse_opt(int key, char *optarg, struct argp_state *state)
{
	char *p;
	enum filter_type ft;
	switch (key) {
		case 'P':
			strncpy(Server_addr.sun_path, optarg, sizeof(Server_addr.sun_path)-1);
			return 0;

		case 'g':
			Group = optarg;
			return 0;

		case 'l':
			p = optarg;
			Rate = strtoul(p, &p, 10);
			if (!Rate || !p || *p++ != '/' || *p == '0')
				argp_error(state, "invalid rate: %s", optarg);
			Rate_period = strtoul(p, &p, 10);
			if (!Rate_period)
				Rate_period = 1;
			switch (*p)
			{
				case 'h':
				case 'H': Rate_period *= 60;
				case 'm':
				case 'M': Rate_period *= 60;
				case 's':
				case 'S': p++;
			}
			if (*p)
				argp_error(state, "unknown rate period: %s\n", p);
			return 0;

		case 'a':
			ft = FILTER_ACCEPT;
			if (0)
		case 'r':
			ft = FILTER_REJECT;
			if (Filter[ft].count == MAX_FILTERS)
				argp_error(state, "too many filters specified\n");
			if (parse_netmask(&Filter[ft].filters[Filter[ft].count], optarg) < 0)
				argp_error(state, "invalid network: %s\n", optarg);
			Filter[ft].count ++;
			return 0;

		default:
			return ARGP_ERR_UNKNOWN;
	}
}

static const struct argp Argp = {
	.options = Options,
	.parser = &parse_opt
};

int main(int argc, char **argv)
{
	if ((Icmp = ping_open()) < 0)
		die("ping_open: %m\n");

	if (setuid(getuid()))
		die("setuid: %m\n");

	srand(getpid() ^ (time(NULL) << 16));
	if ((errno = argp_parse(&Argp, argc, argv, 0, 0, 0)))
		die("argp_parse: %m\n");

	if (signal(SIGTERM, &stop) == SIG_ERR ||
			signal(SIGINT, &stop) == SIG_ERR)
		die("signal: %m\n");
	open_server();

	while (1)
		loop();
}
