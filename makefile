# --- Configuration ---
CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -O3
LDFLAGS  = -lrt -lm
SRC_DIR  = src
BIN_DIR  = .

# --- Files ---
SRCS     = $(SRC_DIR)/l3_overlap_test.c $(SRC_DIR)/PPT.c $(SRC_DIR)/utils.c
OBJS     = $(SRCS:.c=.o)

# --- Targets ---
TARGET   = l3_overlap_test

# --- Rules ---

all: $(TARGET)

# 1. Linking Step:
# Added $(LDFLAGS) here to link the math library
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(BIN_DIR)/$@ $(LDFLAGS)

# 2. Compilation Step:
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean