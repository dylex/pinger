CFLAGS=-Wall -g -O2
CPPFLAGS=-D_GNU_SOURCE=1
BINDIR=/usr/bin

PROGS=pingdev pingerd pinger pingmon pingsize pingstat
default: $(PROGS)

%: %.hs
	ghc -rtsopts -Wall -O --make $@

pingerd pingdev pingsize: ping.o

install: $(PROGS)
	install -o root -m 4755 -t $(BINDIR) pingerd
	install -t $(BINDIR) pinger
	install -o root -m 4755 -t $(BINDIR) pingmon
	install -t $(BINDIR) pingstat
	install -t /usr/sbin pingdev
