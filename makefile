CFLAGS  = -Wall -Wextra -std=gnu23
LDFLAGS = -lm -llua -lmicrohttpd

cc := gcc

config.h: config.def.h
	cp config.def.h config.h

all: main.o config.h
	$(cc) main.o -o interl $(LDFLAGS)

main.o: ketopt.h config.h main.c
	$(cc) -c main.c -o main.o $(CFLAGS)

clean:
	rm -rf *.o interl

fmt:
	find ./ -iname '*.h' -o -iname '*.c' | xargs clang-format -i

PHONY: fmt clean
