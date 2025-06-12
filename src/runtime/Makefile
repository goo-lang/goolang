# Goo Runtime Library Makefile

CC = gcc
CFLAGS = -std=c23 -Wall -Wextra -O2 -fPIC
DEBUG_CFLAGS = -std=c23 -Wall -Wextra -g -O0 -fPIC -DGOO_DEBUG
LDFLAGS = -pthread
INCLUDE_DIR = ../../include
BUILD_DIR = ../../build

# Source files
SOURCES = runtime.c platform.c concurrency.c channels.c sync.c
OBJECTS = $(SOURCES:.c=.o)
DEBUG_OBJECTS = $(SOURCES:.c=.debug.o)

# Output
LIB_NAME = libgooruntime
STATIC_LIB = $(BUILD_DIR)/$(LIB_NAME).a
SHARED_LIB = $(BUILD_DIR)/$(LIB_NAME).so
DEBUG_STATIC_LIB = $(BUILD_DIR)/$(LIB_NAME)_debug.a
DEBUG_SHARED_LIB = $(BUILD_DIR)/$(LIB_NAME)_debug.so

# Default target
all: $(STATIC_LIB) $(SHARED_LIB)

# Debug target
debug: $(DEBUG_STATIC_LIB) $(DEBUG_SHARED_LIB)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

%.debug.o: %.c
	$(CC) $(DEBUG_CFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# Build static library
$(STATIC_LIB): $(OBJECTS) | $(BUILD_DIR)
	ar rcs $@ $(OBJECTS)

# Build shared library
$(SHARED_LIB): $(OBJECTS) | $(BUILD_DIR)
	$(CC) -shared $(LDFLAGS) -o $@ $(OBJECTS)

# Build debug static library
$(DEBUG_STATIC_LIB): $(DEBUG_OBJECTS) | $(BUILD_DIR)
	ar rcs $@ $(DEBUG_OBJECTS)

# Build debug shared library
$(DEBUG_SHARED_LIB): $(DEBUG_OBJECTS) | $(BUILD_DIR)
	$(CC) -shared $(LDFLAGS) -o $@ $(DEBUG_OBJECTS)

# Test target
test: test_runtime test_concurrency

test_runtime: test_runtime.c $(STATIC_LIB)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -o $@ $< -L$(BUILD_DIR) -lgooruntime $(LDFLAGS)

test_concurrency: test_concurrency.c $(STATIC_LIB)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -o $@ $< -L$(BUILD_DIR) -lgooruntime $(LDFLAGS)

test_simple: test_simple.c $(STATIC_LIB)
	$(CC) $(DEBUG_CFLAGS) -I$(INCLUDE_DIR) -o $@ $< -L$(BUILD_DIR) -lgooruntime $(LDFLAGS)

# Clean
clean:
	rm -f *.o *.debug.o
	rm -f $(STATIC_LIB) $(SHARED_LIB) $(DEBUG_STATIC_LIB) $(DEBUG_SHARED_LIB)
	rm -f test_runtime test_concurrency

# Install headers (for integration with code generator)
install-headers: $(INCLUDE_DIR)/runtime.h
	@echo "Runtime headers already in place at $(INCLUDE_DIR)/"

.PHONY: all debug test clean install-headers