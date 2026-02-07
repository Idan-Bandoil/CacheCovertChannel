CC = gcc
INCLUDES = -I/usr/local/include
LDFLAGS = -L/usr/local/lib
LIBS = -lmastik -ldwarf -lelf
CFLAGS = -O3 -Wall $(INCLUDES)

# Source files
SRC_POL_L1 = src/test_l1_policy.c
SRC_POL_L3 = src/test_l3_policy.c

all: test_l1 test_l3

test_l1: $(SRC_POL_L1)
	$(CC) $(CFLAGS) -o test_l1 $(SRC_POL_L1) $(LDFLAGS) $(LIBS)

test_l3: $(SRC_POL_L3)
	$(CC) $(CFLAGS) -o test_l3 $(SRC_POL_L3) $(LDFLAGS) $(LIBS)

clean:
	rm -f test_l1 test_l3