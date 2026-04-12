CC = gcc
CFLAGS = -Wall -Wextra -O2 -march=native -std=c11 -pedantic

SRC_DIR = src
OBJ_DIR = obj

# Executables
TARGET1 = eviction_builder
TARGET2 = negative_control
TARGET3 = mapping_attack

# Object files
UTILS_OBJ = $(OBJ_DIR)/utils.o
MAIN_OBJ = $(OBJ_DIR)/main.o
NEG_OBJ = $(OBJ_DIR)/negative_control.o
MAPPING_OBJ = $(OBJ_DIR)/mapping_attack.o

all: $(TARGET1) $(TARGET2) $(TARGET3)

$(TARGET1): $(MAIN_OBJ) $(UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET2): $(NEG_OBJ) $(UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET3): $(MAPPING_OBJ) $(UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET1) $(TARGET2) $(TARGET3)

.PHONY: all clean