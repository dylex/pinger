#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>
#include <oping.h>
#include <argp.h>
#include <math.h>
#include <time.h>

static const struct argp_option Options[] = 
	{ { "interval",		'i', "SEC", 0,		"interval/timeout between pings [60]" }
	, { "output",		'o', "FILE", 0,		"file to which to write ping data" }
	, { "flush",		's', NULL, 0,		"sync file after each ping result" }
	, { }
	};

static pingobj_t *Ping;
static unsigned Interval = 60;
static FILE *Output;
static bool Flush;
static uid_t Euid;

typedef uint32_t delta_t;
#define DELTA_UNITS	1000000UL
#define DELTA_BIT	(1UL<<(8*sizeof(delta_t)-1))
#define DELTA_THRESH	(DELTA_BIT/DELTA_UNITS)

static void done(int sig) __attribute__((noreturn));
static void done(int sig) 
{
	if (Ping)
		ping_destroy(Ping);
	fclose(Output);
	exit(1);
}

static void die(const char *msg, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	vfprintf(stderr, msg, args);
	va_end(args);
	done(0);
}

static error_t parse(int key, char *optarg, struct argp_state *state)
{
	char *e;
	int r;

	switch (key) {
		case 'i':
			Interval = strtoul(optarg, &e, 10);
			if (!Interval || *e)
				argp_error(state, "invalid interval: %s", optarg);
			return 0;

		case 'o':
			if (!(Output = fopen(optarg, "a")))
				argp_failure(state, 1, errno, "%s", optarg);
			return 0;

		case 's':
			Flush = true;
			return 0;

		case ARGP_KEY_ARG:
			seteuid(Euid);
			r = ping_host_add(Ping, optarg);
			seteuid(getuid());
			if (r < 0)
				argp_failure(state, 1, 0, "%s: %s\n", optarg, ping_get_error(Ping));
			return 0;

		case ARGP_KEY_NO_ARGS:
			argp_usage(state);

		default:
			return ARGP_ERR_UNKNOWN;
	}
}

static const struct argp Parser = {
	.options = Options,
	.parser = &parse,
	.args_doc = "HOST ...",
	.doc = ""
};

static void write_val(delta_t val, const char *msg)
{
	if (fwrite(&val, sizeof(val), 1, Output) != 1)
		die("write %s: %m\n", msg);
}
#define WRITE(VAL) write_val((VAL), #VAL)

int main(int argc, char **argv)
{
	Euid = geteuid();
	if (seteuid(getuid()))
		die("seteuid: %m\n");

	if (!(Ping = ping_construct()))
		die("ping_construct failed\n");

	int af = AF_INET;
	if (ping_setopt(Ping, PING_OPT_AF, &af))
		die("ping af: %s\n", ping_get_error(Ping));

	if ((errno = argp_parse(&Parser, argc, argv, 0, 0, 0)))
		die("argp: %m\n");

	if (!Output)
		Output = stdout;

	if (setuid(getuid()))
		die("setuid: %m\n");

	double intervald = Interval;
	if (ping_setopt(Ping, PING_OPT_TIMEOUT, &intervald))
		die("ping timeout: %s\n", ping_get_error(Ping));

	sigset_t sigset;
	if (sigemptyset(&sigset) || 
			sigaddset(&sigset, SIGTERM) ||
			sigaddset(&sigset, SIGINT))
		die("sigset: %m\n");
	if (signal(SIGTERM, &done) == SIG_ERR ||
			signal(SIGINT, &done) == SIG_ERR)
		die("signal: %m\n");

	pingobj_iter_t *pi;
	unsigned n = 0;
	in_addr_t addr[256];
	for (pi = ping_iterator_get(Ping); pi; pi = ping_iterator_next(pi))
	{
		if (n >= sizeof(addr)/sizeof(*addr))
			die("too many ping targets\n");
		char addrs[16];
		size_t len = sizeof(addrs);
		if ((errno = ping_iterator_get_info(pi, PING_INFO_ADDRESS, addrs, &len) != 0))
			die("get ping addr: %m\n");
		addr[n++] = inet_addr(addrs);
	}

	if (sigprocmask(SIG_BLOCK, &sigset, NULL))
		die("sigblock: %m\n");
	WRITE(n);
	if (fwrite(addr, sizeof(*addr), n, Output) != n)
		die("write addrs: %m\n");
	if (sigprocmask(SIG_UNBLOCK, &sigset, NULL))
		die("sigunblock: %m\n");

	struct timeval last = {};
	while (1)
	{
		struct timeval curr, next = { Interval * (1 + last.tv_sec / Interval) };
		if (gettimeofday(&curr, NULL) < 0)
			die("gettimeofday: %m\n");
		if (timercmp(&curr, &next, <))
		{
			struct timespec diff;
			timersub(&next, &curr, &curr);
			TIMEVAL_TO_TIMESPEC(&curr, &diff);
			if (nanosleep(&diff, NULL) < 0)
				die("nanosleep: %m\n");
			if (gettimeofday(&curr, NULL) < 0)
				die("gettimeofday: %m\n");
		}

		if (ping_send(Ping) < 0)
			die("ping_send: %s\n", ping_get_error(Ping));

		if (sigprocmask(SIG_BLOCK, &sigset, NULL))
			die("sigblock: %m\n");

		struct timeval diff;
		timersub(&curr, &last, &diff);
		delta_t dtime;
		if (!timerisset(&last) || diff.tv_sec >= DELTA_THRESH)
		{
			/* 2038 bug */
			write_val(curr.tv_sec & ~DELTA_BIT, "time");
			dtime = curr.tv_usec;
		}
		else
			dtime = DELTA_BIT | (DELTA_UNITS*diff.tv_sec + diff.tv_usec);
		WRITE(dtime);

		for (pi = ping_iterator_get(Ping); pi; pi = ping_iterator_next(pi))
		{
			double latency;
			size_t len = sizeof(latency);
			if ((errno = ping_iterator_get_info(pi, PING_INFO_LATENCY, &latency, &len) != 0))
				die("get ping latency: %m\n");
			delta_t lat;
			if (latency >= 0)
			{
				latency /= 1000; /* ms */
				lat = DELTA_UNITS*latency;
				if (latency >= DELTA_THRESH)
					lat |= DELTA_BIT;
			}
			else
				lat = ~0;
			WRITE(lat);
		}

		if (sigprocmask(SIG_UNBLOCK, &sigset, NULL))
			die("sigunblock: %m\n");

		if (Flush)
			fflush(Output);

		memcpy(&last, &curr, sizeof(struct timeval));
	}
}
