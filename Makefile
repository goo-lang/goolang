CC = gcc
CFLAGS = -Wall -Wextra -std=c23 -g -Iinclude
LDFLAGS = -lm -pthread

# LLVM configuration - try Homebrew paths first
BREW_LLVM_PATH = $(shell brew --prefix llvm 2>/dev/null)
ifneq ($(BREW_LLVM_PATH),)
    LLVM_CONFIG = $(BREW_LLVM_PATH)/bin/llvm-config
else
    LLVM_CONFIG = llvm-config
endif

LLVM_AVAILABLE = $(shell command -v $(LLVM_CONFIG) 2> /dev/null)
ifdef LLVM_AVAILABLE
    LLVM_CFLAGS = $(shell $(LLVM_CONFIG) --cflags)
    LLVM_LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags --libs core)
    # Add WebAssembly target libraries
    LLVM_LDFLAGS += $(shell $(LLVM_CONFIG) --libs webassembly 2>/dev/null || echo "")
else
    LLVM_CFLAGS = 
    LLVM_LDFLAGS = 
endif

# Directories
SRCDIR = src
INCDIR = include
TESTDIR = tests
BUILDDIR = build
BINDIR = bin

# Source files (lexer + parser + AST + types)
LEXER_SRCS = $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
PARSER_SRCS = $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/lexer_bridge.c
AST_SRCS = $(SRCDIR)/ast/ast.c
TYPES_SRCS = $(SRCDIR)/types/types.c $(SRCDIR)/types/type_checker.c $(SRCDIR)/types/expression_checker.c $(SRCDIR)/types/expression_helpers.c $(SRCDIR)/types/ownership_checker.c $(SRCDIR)/types/channel_checker.c $(SRCDIR)/types/type_test.c
CODEGEN_SRCS = $(SRCDIR)/codegen/codegen.c $(SRCDIR)/codegen/type_mapping.c $(SRCDIR)/codegen/function_codegen.c $(SRCDIR)/codegen/expression_codegen.c $(SRCDIR)/codegen/error_union_codegen.c $(SRCDIR)/codegen/runtime_integration.c $(SRCDIR)/codegen/codegen_test.c $(SRCDIR)/codegen/error_union_test.c $(SRCDIR)/codegen/wasm_codegen.c
RUNTIME_SRCS = $(SRCDIR)/runtime/runtime.c $(SRCDIR)/runtime/platform.c $(SRCDIR)/runtime/concurrency.c $(SRCDIR)/runtime/channels.c $(SRCDIR)/runtime/sync.c $(SRCDIR)/runtime/deadlock.c

CURRENT_SRCS = $(LEXER_SRCS) $(PARSER_SRCS) $(AST_SRCS) $(TYPES_SRCS) $(CODEGEN_SRCS) $(RUNTIME_SRCS)
OBJS = $(CURRENT_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

# Main targets
COMPILER = $(BINDIR)/goo
TEST_RUNNER = $(BINDIR)/test_runner

.PHONY: all clean test install lexer

all: lexer

lexer: $(COMPILER)

# Create directories
$(BUILDDIR) $(BINDIR):
	mkdir -p $@

# Bison rules
$(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.tab.h: $(SRCDIR)/parser/parser.y
	bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y

# Object file compilation
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

# Main compiler
$(COMPILER): $(OBJS) $(SRCDIR)/main.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(SRCDIR)/main.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

# Tests
test: $(TEST_RUNNER)
	./$(TEST_RUNNER)

$(TEST_RUNNER): $(OBJS) $(TESTDIR)/test_main.c | $(BINDIR)
	$(CC) $(CFLAGS) $(TESTDIR)/test_main.c $(OBJS) -o $@ $(LDFLAGS)

# Clean
clean:
	rm -rf $(BUILDDIR) $(BINDIR)
	rm -f $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.tab.h $(SRCDIR)/parser/parser.yy.c

# Install
install: $(COMPILER)
	cp $(COMPILER) /usr/local/bin/

# Development helpers
.PHONY: debug format check
debug: CFLAGS += -DDEBUG -O0
debug: $(COMPILER)

format:
	find $(SRCDIR) $(INCDIR) -name "*.c" -o -name "*.h" | xargs clang-format -i

check:
	cppcheck --enable=all $(SRCDIR)