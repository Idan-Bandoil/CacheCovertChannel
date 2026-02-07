CC = gcc

# Includes
INCLUDES = -I/usr/local/include

# Libraries (Must be linked at the end)
LDFLAGS = -L/usr/local/lib
LIBS = -lmastik -ldwarf -lelf

CFLAGS = -O3 -Wall $(INCLUDES)

SRC_POL = src/test_lru.c

all: test_lru

# The $(LIBS) are now at the end of the line
test_lru: $(SRC_POL)
	$(CC) $(CFLAGS) -o test_lru $(SRC_POL) $(LDFLAGS) $(LIBS)

clean:
	rm -f test_lru
	