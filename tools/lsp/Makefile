CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g -pthread
LDFLAGS = -ljson-c -pthread
TARGET = goo-lsp
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
	@echo "Running LSP tests..."
	@./test_lsp.sh

# Development targets
debug: CFLAGS += -DDEBUG -g3
debug: $(TARGET)

release: CFLAGS += -DNDEBUG -O3
release: clean $(TARGET)

# Check dependencies
deps:
	@echo "Checking dependencies..."
	@pkg-config --exists json-c || (echo "Error: json-c not found. Install with: sudo apt-get install libjson-c-dev" && exit 1)
	@echo "Dependencies OK"

# Example usage
example: $(TARGET)
	@echo "Starting LSP server (use Ctrl+C to stop)..."
	@echo "Send LSP messages to stdin to test the server"
	@./$(TARGET) --trace