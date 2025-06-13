CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
LDFLAGS = 
TARGET = goo-fmt
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
	@echo "Running formatter tests..."
	@./test_formatter.sh

# Development targets
debug: CFLAGS += -DDEBUG -g3
debug: $(TARGET)

release: CFLAGS += -DNDEBUG -O3
release: clean $(TARGET)

# Example usage
example: $(TARGET)
	@echo "Formatting example file..."
	@echo 'package main\nimport "fmt"\nfunc main(){fmt.Println("Hello, World!")}' > example.goo
	@echo "Before formatting:"
	@cat example.goo
	@echo -e "\n\nAfter formatting:"
	@./$(TARGET) example.goo
	@rm -f example.goo