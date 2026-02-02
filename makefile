# Compiler
CC = gcc

# Flags
# -g:   Add debug symbols (helpful for side-channel debugging)
# -O0:  Disable optimization (CRITICAL for side-channel code loops)
CFLAGS = -g -O0 -Wall -Wextra

# Include paths (Adjust if Mastik installed somewhere else)
INCLUDES = -I/usr/local/include

# Library paths
LPATHS = -L/usr/local/lib

# Libraries to link
# -lmastik: The MASTIK toolkit itself
# -ldwarf -lelf: Required for Mastik's symbol resolution features
LIBS = -lmastik -ldwarf -lelf

# Target executable name
TARGET = mastik_demo

# Source files
SRCS = src/main.c

# Build rules
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TARGET) $(SRCS) $(LPATHS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
