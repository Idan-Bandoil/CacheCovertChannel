CC = gcc
CFLAGS = -O3 -Wall
SRC_POL = src/cache_policy_test.c

all: cache_policy_test

cache_policy_test: $(SRC_POL)
	$(CC) $(CFLAGS) -o cache_policy_test $(SRC_POL)

clean:
	rm -f cache_policy_test
