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
	, { "threshold",	't', "COUNT", 0,	"number of consecutive lost to consider \"down\" [1]" }
	, { "down-command",	'd', "CMD", 0,		"run when a host is \"down\"" }
	, { "up-command",	'u', "CMD", 0,		"run when a host is no longer \"down\"" }
	, { }
	};

static pingobj_t *Ping;
static unsigned Interval = 60;
static FILE *Output;
static bool Flush;
static unsigned Threshold = 1;
static const char *Down_cmd, *Up_cmd;
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
	if (Output)
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

		case 't':
			Threshold = strtoul(optarg, &e, 10);
			if (*e)
				argp_error(state, "invalid threshold: %s", optarg);
			return 0;

		case 'o':
			if (!(Output = fopen(optarg, "a")))
				argp_failure(state, 1, errno, "%s", optarg);
			return 0;

		case 's':
			Flush = true;
			return 0;

		case 'd':
			Down_cmd = optarg;
			return 0;

		case 'u':
			Up_cmd = optarg;
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
	.doc = "Monitor the specified hosts using liboping.\v"
		"Output FILE is in space-efficient, appendable binary format, suitable for reading by pingstat.  A host is considered \"down\" once COUNT pings are lost.  Commands are edge-triggered, unless COUNT is 0 in which case the \"down\" command is run for every lost ping.  The following arguments are passed: number of lost pings, host name, host address."
};

static inline void write_val(delta_t val, const char *msg)
{
	if (Output && fwrite(&val, sizeof(val), 1, Output) != 1)
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
			signal(SIGINT, &done) == SIG_ERR ||
			signal(SIGCHLD, SIG_IGN) == SIG_ERR)
		die("signal: %m\n");

	pingobj_iter_t *pi;

	unsigned i, n = 0;
	static in_addr_t addr[256];
	for (pi = ping_iterator_get(Ping); pi; pi = ping_iterator_next(pi))
	{
		if (n >= 256)
			die("too many ping targets\n");
		ping_iterator_set_context(pi, (void *)0);
		char addrs[16] = "";
		size_t len = sizeof(addrs)-1;
		if (ping_iterator_get_info(pi, PING_INFO_ADDRESS, addrs, &len) == 0)
			addr[n] = inet_addr(addrs);
		n ++;
	}

	if (sigprocmask(SIG_BLOCK, &sigset, NULL))
		die("sigblock: %m\n");
	WRITE(n);
	for (i = 0; i < n; i ++)
		WRITE(addr[i]);
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

		if (sigprocmask(SIG_BLOCK, &sigset, NULL))
			die("sigblock: %m\n");
		WRITE(dtime);

		for (pi = ping_iterator_get(Ping); pi; pi = ping_iterator_next(pi))
		{
			intptr_t nd = (intptr_t)ping_iterator_get_context(pi);
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
				if (nd > 0)
					nd = -1;
				else
					nd = 0; 
			}
			else
			{
				lat = ~0;
				if (nd < 0)
					nd = 0;
				nd ++;
			}
			WRITE(lat);
			ping_iterator_set_context(pi, (void *)nd);
		}

		if (sigprocmask(SIG_UNBLOCK, &sigset, NULL))
			die("sigunblock: %m\n");

		if (Output && Flush)
			fflush(Output);

		if (Down_cmd || Up_cmd) for (pi = ping_iterator_get(Ping); pi; pi = ping_iterator_next(pi))
		{
			intptr_t nd = (intptr_t)ping_iterator_get_context(pi);
			const char *cmd = NULL, *type;
			if (Threshold ? nd == Threshold : nd > 0)
			{
				cmd = Down_cmd;
				type = "down";
			}
			else if (nd == -1)
			{
				cmd = Up_cmd;
				type = "up";
			}
			if (cmd && !fork())
			{
				if (Output)
					fclose(Output);
				char count[16] = "";
				char hostname[256] = "";
				char hostaddr[16] = "";
				size_t l;
				snprintf(count, sizeof(count), "%u", nd < 0 ? 0 : (unsigned)nd);
				l = sizeof(hostname)-1;
				ping_iterator_get_info(pi, PING_INFO_USERNAME, hostname, &l);
				l = sizeof(hostaddr)-1;
				ping_iterator_get_info(pi, PING_INFO_ADDRESS, hostaddr, &l);
				ping_destroy(Ping);
				execl("/bin/sh", "sh", "-c", cmd, type, count, hostname, hostaddr, NULL);
				fprintf(stderr, "exec %s cmd: %m\n", type);
				exit(1);
			}
		}

		memcpy(&last, &curr, sizeof(struct timeval));
	}
}
