#include <argp.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/fuse.h>
#include "ping.h"
#include "pingdev.h"

static char Devname[256] = "ping"; 
static unsigned short Interval = 60;
static struct in_addr Target;

static int Cuse = -1;
static int Ping = -1;

static uint16_t Ping_id;
static unsigned Ping_seq;
static bool Ping_wait;
static struct timeval Ping_time;
static float Ping_last = NAN;

#define BUFSIZE 256
static struct reader {
	unsigned seq;
	unsigned off;
	float cur;
	/* active only: */
	uint32_t unique;
	unsigned size;
	struct reader *next, **prev;
} *Readers;

static void stop(int sig) __attribute__((noreturn));
static void stop(int sig) 
{
	if (Cuse >= 0)
		close(Cuse);
	if (Ping >= 0)
		close(Ping);
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

static inline void reader_add(struct reader *r)
{
	struct reader **p = &Readers;

	while (*p && (*p)->seq < r->seq)
		p = &(*p)->next;

	if ((r->next = *p))
		r->next->prev = &r->next;
	r->prev = p;
	*p = r;
}

static inline void reader_del(struct reader *r)
{
	if ((*r->prev = r->next))
		r->next->prev = r->prev;
	r->next = NULL;
	r->prev = NULL;
}

static inline float timeval_diff(const struct timeval *a, const struct timeval *b)
{
	/* assumes no wrap-around, which should be safe with timeouts limited */
	return (a->tv_sec - b->tv_sec) + (a->tv_usec - b->tv_usec)/1e6;
}

static size_t cuse_read(struct fuse_in_header *in, size_t len)
{
	ssize_t r = read(Cuse, in, sizeof(*in)+len);
	if (r < 0)
		die("cuse read: %m\n");
	if (r < sizeof(*in) || r != in->len)
		die("cuse read: invalid message (%zd/%u)\n", r, in->len);
	return r - sizeof(*in);
}

static void cuse_write(struct fuse_out_header *out)
{
	ssize_t r = write(Cuse, out, out->len);
	if (r < 0)
		die("cuse write: %m\n");
	if (r != out->len)
		die("cuse write: short (%zd/%u)\n", r, out->len);
}

static void cuse_init()
{
	size_t l = strnlen(Devname, 255)+1;

	if (l > 255)
		die("cuse: devname too long\n");

	if ((Cuse = open("/dev/cuse", O_RDWR)) < 0)
		die("/dev/cuse: %m\n");

	struct {
		struct fuse_in_header h;
		struct cuse_init_in i;
	} in;
	size_t r = cuse_read(&in.h, sizeof(in.i));
	if (in.h.opcode != CUSE_INIT || r < sizeof(in.i) || in.i.major < FUSE_KERNEL_VERSION)
		die("cuse: bad protocol (%u,%u)\n", in.h.opcode, in.i.major);

	struct {
		struct fuse_out_header h;
		struct cuse_init_out i;
		char devvar[8];
		char devname[256];
	} out = {
		{ .len = sizeof(out)-sizeof(out.devname)+l+1, .unique = in.h.unique },
		{ .major = FUSE_KERNEL_VERSION, .minor = FUSE_KERNEL_MINOR_VERSION
		, .max_read = BUFSIZE, .max_write = BUFSIZE
		, .dev_major = 0, .dev_minor = 0
		},
		"DEVNAME="
	};
	memcpy(out.devname, Devname, l);
	out.devname[l] = 0;
	cuse_write(&out.h);
}

static int pingdev_open(const struct fuse_in_header *h, const struct fuse_open_in *in, size_t len)
{
	if (len < sizeof(*in))
		return EINVAL;

#if __O_LARGEFILE == 0
# undef __O_LARGEFILE
# define __O_LARGEFILE 0100000
#endif
	if ((in->flags & ~__O_LARGEFILE) != O_RDONLY)
	{
		fprintf(stderr, "cuse open: bad flags 0%o\n", in->flags);
		return EPERM;
	}

	struct reader *r = calloc(sizeof(*r), 1);
	r->seq = Ping_seq;

	struct {
		struct fuse_out_header h;
		struct fuse_open_out o;
	} out = {
		{ .len = sizeof(out), .unique = h->unique },
		{ .fh = (intptr_t)r, .open_flags = FOPEN_DIRECT_IO | FOPEN_NONSEEKABLE }
	};

	cuse_write(&out.h);
	return -1;
}

static bool handle_read(struct reader *r)
{
	if (r->seq > Ping_seq)
	{
		if (!r->prev)
			reader_add(r);
		return false;
	}

	if (r->prev)
		reader_del(r);

	if (!r->off)
	{
		r->seq = Ping_seq;
		r->cur = Ping_last;
	}

	static char buffer[BUFSIZE];
	char *buf = buffer;
	int len;

	if (isnan(r->cur))
		len = 0;
	else {
		len = snprintf(buf, BUFSIZE, "%f\n", 1000*r->cur);
		if (len > BUFSIZE)
			len = BUFSIZE;
		if (len < 0)
			len = 0;
	}
	buf += r->off;
	len -= r->off;

	if (len > r->size)
	{
		len = r->size;
		r->off += len;
	}
	else
	{
		r->seq ++;
		r->off = 0;
	}

	if (!len)
	{
		if (!r->prev)
			reader_add(r);
		return true;
	}

	struct {
		struct fuse_out_header h;
		char buf[BUFSIZE];
	} out = {
		{ .len = sizeof(out.h) + len, .unique = r->unique }
	};
	memcpy(out.buf, buf, len);
	cuse_write(&out.h);

	return true;
}

static void handle_readers()
{
	while (Readers && handle_read(Readers));
}

static int pingdev_read(const struct fuse_in_header *h, const struct fuse_read_in *in, size_t len)
{
	if (len < sizeof(*in))
		return EINVAL;

	struct reader *r = (struct reader *)in->fh;
	r->unique = h->unique;
	r->size = in->size;
	handle_read(r);
	return -1;
}

static int pingdev_interrupt(const struct fuse_in_header *h, const struct fuse_interrupt_in *in, size_t len)
{
	if (len < sizeof(*in))
		return EINVAL;

	struct reader *r;
	for (r = Readers; r; r = r->next)
	{
		if (r->unique == in->unique)
		{
			struct fuse_out_header out = { .len = sizeof(out), .error = -EINTR, .unique = r->unique };
			cuse_write(&out);
			reader_del(r);
			return -1;
		}
	}

	return EBADF;
}

static int pingdev_release(const struct fuse_in_header *h, const struct fuse_release_in *in, size_t len)
{
	if (len < sizeof(*in))
		return EINVAL;

	struct reader *r = (struct reader *)in->fh;
	if (r->prev)
		reader_del(r);
	free(r);
	return 0;
}

static void cuse_in()
{
	struct {
		struct fuse_in_header h;
		char buf[1024];
	} in;
	size_t r = cuse_read(&in.h, sizeof(in.buf));

	int err;
	switch (in.h.opcode)
	{
		case FUSE_DESTROY:
			die("cuse destroyed");
		case FUSE_OPEN:
			err = pingdev_open(&in.h, (struct fuse_open_in *)in.buf, r);
			break;
		case FUSE_READ:
			err = pingdev_read(&in.h, (struct fuse_read_in *)in.buf, r);
			break;
		case FUSE_INTERRUPT:
			err = pingdev_interrupt(&in.h, (struct fuse_interrupt_in *)in.buf, r);
			break;
		case FUSE_RELEASE:
			err = pingdev_release(&in.h, (struct fuse_release_in *)in.buf, r);
			break;
		case FUSE_IOCTL:
			err = ENOTTY;
			break;
		default:
			fprintf(stderr, "cuse: unhandled opcode %u\n", in.h.opcode);
			err = ENOSYS;
	}
	if (err >= 0)
	{
		struct fuse_out_header out = { sizeof(out), .error = -err, .unique = in.h.unique };
		cuse_write(&out);
	}
}

static void ping_update(float t)
{
	Ping_last = t;
	Ping_wait = false;
	Ping_seq ++;

	handle_readers();
}

static void ping_in()
{
	uint16_t id, seq;
	struct in_addr host;
	struct timeval t = {};
	int r = ping_recv(Ping, &id, &seq, &host, &t);
	if (r < 0)
		die("ping recv: %m\n");
	if (!r) 
		return;
	if (id != Ping_id || host.s_addr != Target.s_addr)
		return;
	if (!timerisset(&t))
		gettimeofday(&t, NULL);
	if (seq == (uint16_t)Ping_seq)
	{
		ping_update(timeval_diff(&t, &Ping_time));
	}
	else if (seq == (uint16_t)Ping_seq-1 && isinf(Ping_last) == 1)
	{
		Ping_last = timeval_diff(&t, &Ping_time) + Interval;
	}
}

static void loop()
{
	struct timeval now, diff;
	gettimeofday(&now, NULL);
	timersub(&now, &Ping_time, &diff);
	if (diff.tv_sec >= Interval)
	{
		if (Ping_wait)
			ping_update(INFINITY);

		timerclear(&diff);
		Ping_time = now;
		ping_send(Ping, Ping_id, Ping_seq, Target);
		Ping_wait = true;
	}

	struct pollfd polls[2] = 
		{ { .fd = Cuse, .events = POLLIN }
		, { .fd = Ping, .events = POLLIN }
		};
	int r = poll(polls, 2, 1000*(Interval - diff.tv_sec) - diff.tv_usec/1000);
	if (r < 0)
		die("poll: %m\n");
	if (polls[0].revents)
		cuse_in();
	if (polls[1].revents)
		ping_in();
}

static const struct argp_option Options[] = 
	{ { "devname", 'd', "NAME", 0, "use character device /dev/NAME [ping]" }
	, { "interval", 'i', "SECS", 0, "interval/timeout between pings [60]" }
	, { }
	};

static error_t parse_opt(int key, char *optarg, struct argp_state *state)
{
	char *p;

	switch (key) {
		case 'd':
			strncpy(Devname, optarg, sizeof(Devname));
			return 0;

		case 'i':
			Interval = strtoul(optarg, &p, 10);
			if (!Interval || *p)
				argp_error(state, "invalid interval: %s", optarg);
			return 0;

		case ARGP_KEY_ARG:
		{
			if (Target.s_addr || !inet_aton(optarg, &Target))
				argp_error(state, "invalid host: %s", optarg);
			return 0;
		}

		case ARGP_KEY_NO_ARGS:
			argp_usage(state);

		default:
			return ARGP_ERR_UNKNOWN;
	}
}

static const struct argp Argp = {
	.options = Options,
	.parser = &parse_opt,
	.args_doc = "HOST ...",
	.doc = "Create a character device providing an ICMP ping interface."
};

int main(int argc, char **argv)
{
	if ((Ping = ping_open()) < 0)
		die("ping_open: %m\n");

	uid_t uid = getuid();
	/*
	if (uid == 0)
		uid = 65534;
	*/
	if (setuid(uid))
		die("setuid: %m\n");

	srand(getpid() ^ (intptr_t)*argv);
	if ((errno = argp_parse(&Argp, argc, argv, 0, 0, 0)))
		die("argp_parse: %m\n");

	cuse_init();

	if (signal(SIGTERM, &stop) == SIG_ERR ||
			signal(SIGINT, &stop) == SIG_ERR)
		die("signal: %m\n");

	Ping_id = rand();
	while (1)
		loop();
}
