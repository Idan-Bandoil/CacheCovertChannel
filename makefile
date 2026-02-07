# --- Configuration ---
CC       = gcc
CFLAGS   = -O3 -pthread
SRC_DIR  = src
BIN_DIR  = .

# --- Targets ---
TARGETS  = l3_overlap_test

# --- Rules ---

all: $(TARGETS)

l3_overlap_test: $(SRC_DIR)/l3_overlap_test.c
	$(CC) $(CFLAGS) $< -o $(BIN_DIR)/$@

clean:
	rm -f $(TARGETS)

.PHONY: all clean run
