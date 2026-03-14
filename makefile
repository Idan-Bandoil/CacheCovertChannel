CC = gcc
CFLAGS = -Wall -Wextra -O2 -march=native

SRC_DIR = src
OBJ_DIR = obj

# List all C files in the src directory and generate corresponding .o files
SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/utils.c
OBJS = $(OBJ_DIR)/main.o $(OBJ_DIR)/utils.o

# The final executable name
TARGET = eviction_builder

all: $(TARGET)

# Link the object files to create the final executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Compile .c files into .o files inside the obj directory
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create the object directory if it doesn't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

.PHONY: all clean