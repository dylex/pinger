CFLAGS=-Wall -g -O2
CPPFLAGS=-D_GNU_SOURCE=1
LDFLAGS=-loping
BINDIR=/usr/bin

default: pingmon pingstat

%: %.hs
	ghc -rtsopts -Wall -O --make $@

install: pingmon pingstat
	install -o root -m 4755 -t $(BINDIR) pingmon
	install -t $(BINDIR) pingstat
