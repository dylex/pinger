#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "pinger.h"

static void die(const char *msg, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	vfprintf(stderr, msg, args);
	va_end(args);
	exit(1);
}

int main(int argc, char **argv)
{
	int s, i, n = 0;
	struct sockaddr_un la = { AF_UNIX }, sa = { AF_UNIX, PINGER_SOCKET };

	if ((s = socket(PF_UNIX, SOCK_DGRAM, 0)) < 0)
		die("socket: %m\n");
	if (bind(s, &la, SUN_LEN(&la)) < 0)
		die("bind: %m\n");
	if (connect(s, &sa, SUN_LEN(&sa)) < 0)
		die("connect: %m\n");
	for (i = 1; i < argc; i ++)
	{
		struct in_addr a;
		if (!inet_aton(argv[i], &a))
		{
			fprintf(stderr, "invalid IP: %s\n", argv[i]);
			continue;
		}
		struct ping p = { a.s_addr, 5000000 };
		if (send(s, &p, sizeof(p), 0) < 0)
			die("send: %m\n");
		n ++;
	}
	for (i = 0; i < n; i ++)
	{
		struct ping p = {};
		if (recv(s, &p, sizeof(p), 0) < 0)
			die("recv: %m\n");
		struct in_addr a = { p.host };
		printf("%s: %d\n", inet_ntoa(a), p.time);
	}
	close(s);
	return 0;
}
