CC = gcc
CFLAGS = -g -O0 -Wall -I/usr/local/include -I./src
LDFLAGS = -L/usr/local/lib -lmastik -ldwarf -lelf

all: sender receiver main

sender: src/sender.c src/common.h
	$(CC) $(CFLAGS) -o sender src/sender.c $(LDFLAGS)

receiver: src/receiver.c src/common.h
	$(CC) $(CFLAGS) -o receiver src/receiver.c $(LDFLAGS)

main: src/main.c
	$(CC) $(CFLAGS) -o main src/main.c $(LDFLAGS)

clean:
	rm -f sender receiver main

.PHONY: all clean
