CC = gcc
CFLAGS = -g -O3 -std=c11 -Wall -I/usr/local/include -I./src
LDFLAGS = -lrt -lm

all: main

main: src/main.c
	$(CC) $(CFLAGS) -o main src/main.c $(LDFLAGS)

clean:
	rm -f main

.PHONY: all clean
