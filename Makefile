CFLAGS=-Wall -g -O2
CPPFLAGS=-D_GNU_SOURCE=1
LDFLAGS=-loping

default: pingmon pingstat

%: %.hs
	ghc -rtsopts -Wall -O --make $@
