CC = gcc
CFLAGS = -O3 -Wall
SRC = src/latency_profiler.c
TARGET = latency_profiler

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
