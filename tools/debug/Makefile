CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
LDFLAGS = -ldwarf -lelf
TARGET = goo-debug
SRCDIR = .
SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

install: $(TARGET)
	mkdir -p $(PREFIX)/bin
	cp $(TARGET) $(PREFIX)/bin/
	chmod +x $(PREFIX)/bin/$(TARGET)

test: $(TARGET)
	@echo "Running debugger tests..."
	@./test_debugger.sh

# Development targets
debug: CFLAGS += -DDEBUG -g3
debug: $(TARGET)

release: CFLAGS += -DNDEBUG -O3
release: clean $(TARGET)

# Check dependencies
deps:
	@echo "Checking dependencies..."
	@pkg-config --exists libdwarf || (echo "Error: libdwarf not found. Install with: sudo apt-get install libdwarf-dev" && exit 1)
	@pkg-config --exists libelf || (echo "Error: libelf not found. Install with: sudo apt-get install libelf-dev" && exit 1)
	@echo "Dependencies OK"

# Create test program
test-program: test_program.c
	$(CC) -g -O0 -o test_program test_program.c

# Example usage
example: $(TARGET) test-program
	@echo "Starting debugger with test program..."
	@echo "Use commands like: break main, run, step, continue, quit"
	@./$(TARGET) test_program