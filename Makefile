CC = gcc
CFLAGS = -Wall -Wextra -std=c23 -g -Iinclude
LDFLAGS = -lm -pthread

# Coverage flags
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
COVERAGE_LIBS = -lgcov

# LLVM configuration - prefer system LLVM 20, then Homebrew LLVM
LLVM_CONFIG = $(shell which llvm-config-20 2>/dev/null || which llvm-config 2>/dev/null || echo "/opt/homebrew/opt/llvm/bin/llvm-config")

ifeq ($(shell test -x $(LLVM_CONFIG) && echo "exists"), exists)
    LLVM_CFLAGS = $(shell $(LLVM_CONFIG) --cflags) -DLLVM_AVAILABLE=1
    LLVM_LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags --libs core)
    # Add WebAssembly target libraries if available
    LLVM_LDFLAGS += $(shell $(LLVM_CONFIG) --libs webassembly 2>/dev/null || echo "")
    $(info Using LLVM config: $(LLVM_CONFIG))
    $(info LLVM version: $(shell $(LLVM_CONFIG) --version))
else
    LLVM_CFLAGS = -DLLVM_AVAILABLE=0
    LLVM_LDFLAGS = 
    $(warning LLVM not found at $(LLVM_CONFIG) - building without LLVM support)
endif

# Directories
SRCDIR = src
INCDIR = include
TESTDIR = tests
BUILDDIR = build
BINDIR = bin

# Test subdirectories
TEST_UNIT_DIR = $(TESTDIR)/unit
TEST_INTEGRATION_DIR = $(TESTDIR)/integration
TEST_FRAMEWORK_DIR = $(TESTDIR)/framework
TEST_DEMOS_DIR = $(TESTDIR)/demos

# Source files (lexer + parser + AST + types + error handling + test framework)
LEXER_SRCS = $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
PARSER_SRCS = $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/lexer_bridge.c $(SRCDIR)/parser/parser_errors.c
AST_SRCS = $(SRCDIR)/ast/ast.c
TYPES_SRCS = $(SRCDIR)/types/types.c $(SRCDIR)/types/type_checker.c $(SRCDIR)/types/expression_checker.c $(SRCDIR)/types/expression_helpers.c $(SRCDIR)/types/ownership_checker.c $(SRCDIR)/types/channel_checker.c $(SRCDIR)/types/constraint_inference.c $(SRCDIR)/types/advanced_constraint_inference.c $(SRCDIR)/types/concept_generics.c $(SRCDIR)/types/higher_kinded_types.c $(SRCDIR)/types/type_level_programming.c $(SRCDIR)/types/interface_integration.c $(SRCDIR)/types/flow_sensitive_analysis.c $(SRCDIR)/types/flow_analysis_core.c $(SRCDIR)/types/reference_manager.c $(SRCDIR)/types/hkt_auto_impl.c $(SRCDIR)/types/protocol_oriented_programming.c $(SRCDIR)/types/escape_analysis.c $(SRCDIR)/types/resource_manager.c $(SRCDIR)/types/memory_safety_integration.c $(SRCDIR)/types/bounds_verifier.c $(SRCDIR)/types/dependent_types.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/proof_generation.c $(SRCDIR)/types/runtime_optimization.c
CODEGEN_SRCS = $(SRCDIR)/codegen/codegen.c $(SRCDIR)/codegen/type_mapping.c $(SRCDIR)/codegen/function_codegen.c $(SRCDIR)/codegen/expression_codegen.c $(SRCDIR)/codegen/error_union_codegen.c $(SRCDIR)/codegen/runtime_integration.c $(SRCDIR)/codegen/wasm_codegen.c
RUNTIME_SRCS = $(SRCDIR)/runtime/runtime.c $(SRCDIR)/runtime/platform.c $(SRCDIR)/runtime/concurrency.c $(SRCDIR)/runtime/channels.c $(SRCDIR)/runtime/sync.c $(SRCDIR)/runtime/deadlock.c
ERROR_SRCS = $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c
IDE_SRCS = $(SRCDIR)/ide/hot_reload.c $(SRCDIR)/ide/repl.c $(SRCDIR)/ide/performance_monitor.c $(SRCDIR)/ide/repl_errors.c $(SRCDIR)/ide/time_travel_debug.c $(SRCDIR)/ide/time_travel_debug_repl.c $(SRCDIR)/ide/repl_syntax.c
TEST_FRAMEWORK_SRCS = $(TEST_FRAMEWORK_DIR)/test_framework.c

COMPTIME_SRCS = $(SRCDIR)/comptime/comptime.c $(SRCDIR)/comptime/comptime_types.c $(SRCDIR)/comptime/optimization.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/derive_macros.c $(SRCDIR)/template_macros.c
CURRENT_SRCS = $(LEXER_SRCS) $(PARSER_SRCS) $(AST_SRCS) $(TYPES_SRCS) $(CODEGEN_SRCS) $(RUNTIME_SRCS) $(ERROR_SRCS) $(IDE_SRCS) $(COMPTIME_SRCS)
SRC_OBJS = $(CURRENT_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
TEST_FRAMEWORK_OBJ = $(TEST_FRAMEWORK_SRCS:$(TEST_FRAMEWORK_DIR)/%.c=$(BUILDDIR)/framework/%.o)
OBJS = $(SRC_OBJS) $(TEST_FRAMEWORK_OBJ)

# Main targets
COMPILER = $(BINDIR)/goo
ANALYZER = $(BINDIR)/goo-analyzer
TEST_RUNNER = $(BINDIR)/test_runner
REPL = $(BINDIR)/goo-repl
REPL_ENHANCED = $(BINDIR)/goo-repl-enhanced
LSP_SERVER = $(BINDIR)/goo-lsp
LSP_ENHANCED_SERVER = $(BINDIR)/goo-lsp-enhanced
LSP_STANDALONE_SERVER = $(BINDIR)/goo-lsp-standalone
TEST_REPL = $(BINDIR)/test_repl
TEST_PERFORMANCE = $(BINDIR)/test_performance
TEST_ERROR_REPORTING = $(BINDIR)/test_error_reporting

.PHONY: all clean test install lexer analyzer test-interface test-repl repl repl-enhanced lsp coverage coverage-report coverage-clean debug format check

all: lexer

lexer: $(COMPILER)

# Minimal analyzer (lexer only)
analyzer: $(ANALYZER)

$(ANALYZER): $(LEXER_SRCS) $(SRCDIR)/main_minimal.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^

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

# Test framework object compilation
$(BUILDDIR)/framework/%.o: $(TEST_FRAMEWORK_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

# Main compiler
$(COMPILER): $(OBJS) $(SRCDIR)/main_simple.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(SRCDIR)/main_simple.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

# Test targets
TEST_INTERFACE_SYSTEM = $(BINDIR)/test_interface_system
TEST_FLOW_ANALYSIS = $(BINDIR)/test_flow_analysis
TEST_REFERENCE_MANAGER = $(BINDIR)/test_reference_manager

# Tests
test: $(TEST_RUNNER)
	./$(TEST_RUNNER)

$(TEST_RUNNER): $(OBJS) $(TEST_FRAMEWORK_DIR)/test_main.c $(TEST_UNIT_DIR)/constraint/constraint_inference_test.c $(TEST_UNIT_DIR)/type_system/concept_generics_test.c $(TEST_UNIT_DIR)/type_system/higher_kinded_types_test.c $(TEST_UNIT_DIR)/type_system/concept_declaration_test.c $(TEST_UNIT_DIR)/constraint/advanced_constraint_inference_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(TEST_FRAMEWORK_DIR)/test_main.c $(TEST_UNIT_DIR)/constraint/constraint_inference_test.c $(TEST_UNIT_DIR)/type_system/concept_generics_test.c $(TEST_UNIT_DIR)/type_system/higher_kinded_types_test.c $(TEST_UNIT_DIR)/type_system/concept_declaration_test.c $(TEST_UNIT_DIR)/constraint/advanced_constraint_inference_test.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

# Individual test targets
test-reference: $(TEST_REFERENCE_MANAGER)
	./$(TEST_REFERENCE_MANAGER)

$(TEST_REFERENCE_MANAGER): $(OBJS) $(TEST_UNIT_DIR)/memory/reference_manager_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(TEST_UNIT_DIR)/memory/reference_manager_test.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS) -DSTANDALONE_TEST

# Hot reload test
test-hot-reload: $(OBJS) $(TEST_INTEGRATION_DIR)/hot_reload_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(TEST_INTEGRATION_DIR)/hot_reload_test.c $(OBJS) -o $(BINDIR)/test_hot_reload $(LDFLAGS) $(LLVM_LDFLAGS) -ldl
	./$(BINDIR)/test_hot_reload

# Clean
clean:
	rm -rf $(BUILDDIR) $(BINDIR)
	rm -f $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.tab.h $(SRCDIR)/parser/parser.yy.c

test-interface: $(TEST_INTERFACE_SYSTEM)
	./$(TEST_INTERFACE_SYSTEM)

$(TEST_INTERFACE_SYSTEM): $(TEST_UNIT_DIR)/interface/test_interface_system.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

test-flow: $(TEST_FLOW_ANALYSIS)
	./$(TEST_FLOW_ANALYSIS)

$(TEST_FLOW_ANALYSIS): $(TEST_UNIT_DIR)/flow/flow_analysis_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# REPL targets
repl: $(REPL)

$(REPL): $(SRCDIR)/ide/repl_main.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Enhanced REPL with syntax highlighting
repl-enhanced: $(REPL_ENHANCED)

$(REPL_ENHANCED): $(SRCDIR)/ide/repl_enhanced_simple.c $(SRCDIR)/ide/repl_syntax.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

# Development Workflow Tools
PROJECT_WIZARD = $(BINDIR)/goo-wizard
TEST_RUNNER_TOOL = $(BINDIR)/goo-test
PROFILER_TOOL = $(BINDIR)/goo-profiler
DOC_GENERATOR = $(BINDIR)/goo-docs
HEALTH_DASHBOARD = $(BINDIR)/goo-health

# Complete development workflow toolchain
dev-tools: wizard test-tool profiler doc-generator health-dashboard

# Project template wizard
wizard: $(PROJECT_WIZARD)

$(PROJECT_WIZARD): tools/project_wizard/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Enhanced test runner with visualization
test-tool: $(TEST_RUNNER_TOOL)

$(TEST_RUNNER_TOOL): tools/test_runner/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< -lpthread

# Integrated profiler
profiler: $(PROFILER_TOOL)

$(PROFILER_TOOL): tools/profiler/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< -lpthread

# Documentation generator
doc-generator: $(DOC_GENERATOR)

$(DOC_GENERATOR): tools/doc_generator/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Project health dashboard
health-dashboard: $(HEALTH_DASHBOARD)

$(HEALTH_DASHBOARD): tools/health_dashboard/main.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< -lpthread

# Taint Analysis System Test
TAINT_ANALYSIS_TEST = $(BINDIR)/taint_analysis_test
TAINT_ANALYSIS_SOURCES = src/types/taint_analysis.c src/security/security_framework.c src/errors/error.c

test-taint-analysis: $(TAINT_ANALYSIS_TEST)
	@echo "Running taint analysis system tests..."
	./$(TAINT_ANALYSIS_TEST)

$(TAINT_ANALYSIS_TEST): taint_analysis_test.c $(TAINT_ANALYSIS_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Capability Security System Test
CAPABILITY_SECURITY_TEST = $(BINDIR)/capability_security_test
CAPABILITY_SECURITY_SOURCES = src/security/capability_security.c src/security/security_framework.c src/errors/error.c

test-capability-security: $(CAPABILITY_SECURITY_TEST)
	@echo "Running capability security system tests..."
	./$(CAPABILITY_SECURITY_TEST)

$(CAPABILITY_SECURITY_TEST): capability_security_test.c $(CAPABILITY_SECURITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Security Auditing System Test
SECURITY_AUDITING_TEST = $(BINDIR)/security_auditing_test
SECURITY_AUDITING_SOURCES = src/security/security_auditing.c src/security/security_patterns.c src/security/security_framework.c src/security/capability_security.c src/types/taint_analysis.c src/errors/error.c

test-security-auditing: $(SECURITY_AUDITING_TEST)
	@echo "Running security auditing system tests..."
	./$(SECURITY_AUDITING_TEST)

$(SECURITY_AUDITING_TEST): security_auditing_test.c $(SECURITY_AUDITING_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Cryptographic Security System Test
CRYPTO_SECURITY_TEST = $(BINDIR)/crypto_security_test
CRYPTO_SECURITY_SOURCES = src/security/crypto_security.c src/security/security_framework.c src/errors/error.c

test-crypto-security: $(CRYPTO_SECURITY_TEST)
	@echo "Running cryptographic security system tests..."
	./$(CRYPTO_SECURITY_TEST)

$(CRYPTO_SECURITY_TEST): crypto_security_simple_test.c $(CRYPTO_SECURITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Ergonomic error handling tests
ERGONOMIC_ERROR_TEST = $(BINDIR)/test_ergonomic_errors

test-ergonomic-errors: $(ERGONOMIC_ERROR_TEST)
	./$(ERGONOMIC_ERROR_TEST)

$(ERGONOMIC_ERROR_TEST): $(TEST_UNIT_DIR)/error/ergonomic_errors_test.c $(ERROR_SRCS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# LSP Server targets
lsp: $(LSP_SERVER)

$(LSP_SERVER): $(SRCDIR)/ide/lsp_simple.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Enhanced LSP Server with AST integration
lsp-enhanced: $(LSP_ENHANCED_SERVER)

$(LSP_ENHANCED_SERVER): $(SRCDIR)/ide/lsp_enhanced.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Standalone Enhanced LSP Server (no dependencies)
lsp-standalone: $(LSP_STANDALONE_SERVER)

$(LSP_STANDALONE_SERVER): $(SRCDIR)/ide/lsp_standalone.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Debug Adapter Protocol (DAP) Server
DEBUG_ADAPTER_SERVER = $(BINDIR)/goo-debug-adapter

debug-adapter: $(DEBUG_ADAPTER_SERVER)

$(DEBUG_ADAPTER_SERVER): $(SRCDIR)/ide/debug_adapter.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

# Performance Dashboard Server
PERFORMANCE_DASHBOARD_SERVER = $(BINDIR)/goo-dashboard

dashboard: $(PERFORMANCE_DASHBOARD_SERVER)

$(PERFORMANCE_DASHBOARD_SERVER): $(SRCDIR)/ide/dashboard_main.c $(SRCDIR)/ide/performance_dashboard.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

# Async Streams Test
ASYNC_STREAMS_TEST = $(BINDIR)/async_streams_test
ASYNC_STREAMS_SOURCES = src/async/async_streams.c src/errors/error.c

test-async-streams: $(ASYNC_STREAMS_TEST)
	@echo "Running async streams system tests..."
	./$(ASYNC_STREAMS_TEST)

$(ASYNC_STREAMS_TEST): tests/concurrency/async_streams_test.c $(ASYNC_STREAMS_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test-repl: $(TEST_REPL)
	./$(TEST_REPL)

$(TEST_REPL): $(TEST_INTEGRATION_DIR)/repl_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

test-performance: $(TEST_PERFORMANCE)
	./$(TEST_PERFORMANCE)

$(TEST_PERFORMANCE): $(TEST_INTEGRATION_DIR)/performance_monitor_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

test-error-reporting: $(TEST_ERROR_REPORTING)
	./$(TEST_ERROR_REPORTING)

$(TEST_ERROR_REPORTING): $(TEST_INTEGRATION_DIR)/error_reporting_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Time-travel debugging test
TEST_TIME_TRAVEL_DEBUG = $(BINDIR)/test_time_travel_debug

test-time-travel-debug: $(TEST_TIME_TRAVEL_DEBUG)
	./$(TEST_TIME_TRAVEL_DEBUG)

$(TEST_TIME_TRAVEL_DEBUG): $(TEST_INTEGRATION_DIR)/time_travel_debug_test.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Install
install: $(COMPILER)
	cp $(COMPILER) /usr/local/bin/

# Development helpers
debug: CFLAGS += -DDEBUG -O0
debug: $(COMPILER)

format:
	find $(SRCDIR) $(INCDIR) -name "*.c" -o -name "*.h" | xargs clang-format -i

check:
	cppcheck --enable=all $(SRCDIR)

# Coverage targets
COVERAGE_DIR = coverage
COVERAGE_TEST_RUNNER = $(BINDIR)/test_runner_coverage

coverage: coverage-clean $(COVERAGE_TEST_RUNNER)
	@echo "Running tests with coverage..."
	./$(COVERAGE_TEST_RUNNER)
	@echo "Generating coverage report..."
	lcov --capture --directory $(BUILDDIR) --output-file $(COVERAGE_DIR)/coverage.info
	lcov --remove $(COVERAGE_DIR)/coverage.info '/usr/*' --output-file $(COVERAGE_DIR)/coverage.info
	lcov --remove $(COVERAGE_DIR)/coverage.info '*/test/*' --output-file $(COVERAGE_DIR)/coverage.info
	genhtml $(COVERAGE_DIR)/coverage.info --output-directory $(COVERAGE_DIR)/html
	@echo "Coverage report generated in $(COVERAGE_DIR)/html/index.html"

$(COVERAGE_TEST_RUNNER): $(TEST_FRAMEWORK_DIR)/test_main.c | $(BINDIR) $(COVERAGE_DIR)
	@echo "Building test runner with coverage support..."
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) $(LLVM_CFLAGS) -c -o $(BUILDDIR)/test_main_coverage.o $(TEST_FRAMEWORK_DIR)/test_main.c
	@# Build objects with coverage
	@for src in $(CURRENT_SRCS); do \
		obj=$$(echo $$src | sed 's|$(SRCDIR)/|$(BUILDDIR)/|g' | sed 's|\.c$$|_coverage.o|g'); \
		mkdir -p $$(dirname $$obj); \
		$(CC) $(CFLAGS) $(COVERAGE_FLAGS) $(LLVM_CFLAGS) -c $$src -o $$obj; \
	done
	@# Link with coverage
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) $(BUILDBIN)/test_main_coverage.o $(filter-out $(BUILDDIR)/main.o, $(OBJS:_coverage.o=)) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

coverage-report:
	@echo "Opening coverage report..."
	open $(COVERAGE_DIR)/html/index.html

coverage-clean:
	rm -rf $(COVERAGE_DIR)

# Proof generation test
proof_generation_test: $(TEST_UNIT_DIR)/proof/proof_generation_test.c $(SRCDIR)/types/proof_generation.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/dependent_types.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Runtime optimization framework tests
runtime_optimization_test: $(TEST_UNIT_DIR)/runtime/runtime_optimization_test.c $(SRCDIR)/types/runtime_optimization.c $(SRCDIR)/types/proof_generation.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/dependent_types.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

runtime_optimization_test_simple: $(TEST_UNIT_DIR)/runtime/runtime_optimization_test_simple.c $(SRCDIR)/types/runtime_optimization_simple.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

runtime_optimization_demo: $(TEST_DEMOS_DIR)/runtime_optimization_demo.c $(SRCDIR)/types/runtime_optimization.c $(SRCDIR)/types/proof_generation.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/dependent_types.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

runtime_optimization_demo_simple: $(TEST_DEMOS_DIR)/runtime_optimization_demo_simple.c $(SRCDIR)/types/runtime_optimization_simple.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Contract programming framework tests
contracts_test: $(TEST_UNIT_DIR)/contract/contracts_test.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/dependent_types.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Contract proof integration test
contract_proof_integration_test: $(TEST_UNIT_DIR)/contract/contract_proof_integration_test.c $(SRCDIR)/types/contracts.c $(SRCDIR)/types/proof_generation.c $(SRCDIR)/types/dependent_types.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Actor System Test
ACTOR_SYSTEM_TEST = $(BINDIR)/actor_system_test
ACTOR_SYSTEM_SOURCES = src/runtime/actor_system.c src/errors/error.c

test-actor-system: $(ACTOR_SYSTEM_TEST)
	@echo "Running actor system tests..."
	./$(ACTOR_SYSTEM_TEST)

$(ACTOR_SYSTEM_TEST): tests/concurrency/actor_system_test.c $(ACTOR_SYSTEM_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Shared Variables Test (Task 21.2)
shared_variables_test: tests/concurrency/shared_variables_test.c $(SRCDIR)/concurrency/shared_variables.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/errors/error.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Structured Concurrency Test (Task 21.3)
structured_concurrency_test: tests/concurrency/structured_concurrency_test.c $(SRCDIR)/concurrency/structured_concurrency.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/errors/error.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# All optimization system tests
.PHONY: test-optimization test-optimization-simple test-all-optimization clean-tests
test-optimization: runtime_optimization_test
	@echo "Running runtime optimization tests..."
	./runtime_optimization_test

test-optimization-simple: runtime_optimization_test_simple
	@echo "Running simplified runtime optimization tests..."
	./runtime_optimization_test_simple

test-all-optimization: runtime_optimization_test runtime_optimization_demo contracts_test contract_proof_integration_test proof_generation_test
	@echo "Running all optimization system tests..."
	./runtime_optimization_test
	./contracts_test
	./contract_proof_integration_test
	./proof_generation_test
	@echo "Running runtime optimization demonstration..."
	./runtime_optimization_demo

clean-tests:
	rm -f runtime_optimization_test runtime_optimization_demo contracts_test contract_proof_integration_test proof_generation_test
	rm -f comptime_test comptime_types_test optimization_test pgo_test advanced_optimization_test advanced_macro_test derive_macro_test template_macro_test
	rm -f shared_variables_test structured_concurrency_test
# Work-Stealing Test
WORK_STEALING_TEST = $(BINDIR)/work_stealing_test
WORK_STEALING_SOURCES = src/concurrency/work_stealing.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-work-stealing: $(WORK_STEALING_TEST)
	@echo "Running work-stealing tests..."
	./$(WORK_STEALING_TEST)

$(WORK_STEALING_TEST): work_stealing_test.c $(WORK_STEALING_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Work-Stealing Demo
WORK_STEALING_DEMO = $(BINDIR)/work_stealing_demo

demo-work-stealing: $(WORK_STEALING_DEMO)
	@echo "Running work-stealing demonstration..."
	./$(WORK_STEALING_DEMO)

$(WORK_STEALING_DEMO): tests/examples/work_stealing_demo.c $(WORK_STEALING_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Dynamic Chunking Test
DYNAMIC_CHUNKING_TEST = $(BINDIR)/dynamic_chunking_test
DYNAMIC_CHUNKING_SOURCES = src/concurrency/dynamic_chunking.c src/concurrency/work_stealing.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-dynamic-chunking: $(DYNAMIC_CHUNKING_TEST)
	@echo "Running dynamic chunking tests..."
	./$(DYNAMIC_CHUNKING_TEST)

$(DYNAMIC_CHUNKING_TEST): dynamic_chunking_test.c $(DYNAMIC_CHUNKING_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Memory Safety Test
MEMORY_SAFETY_TEST = $(BINDIR)/memory_safety_test
MEMORY_SAFETY_SOURCES = src/concurrency/parallel_memory_safety.c src/concurrency/work_stealing.c src/concurrency/dynamic_chunking.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-memory-safety: $(MEMORY_SAFETY_TEST)
	@echo "Running memory safety tests..."
	./$(MEMORY_SAFETY_TEST)

$(MEMORY_SAFETY_TEST): memory_safety_test.c $(MEMORY_SAFETY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Performance Monitoring Test
PERFORMANCE_MONITORING_TEST = $(BINDIR)/performance_monitoring_test
PERFORMANCE_MONITORING_SOURCES = src/concurrency/performance_monitoring.c src/concurrency/parallel_memory_safety.c src/concurrency/work_stealing.c src/concurrency/dynamic_chunking.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-performance-monitoring: $(PERFORMANCE_MONITORING_TEST)
	@echo "Running performance monitoring tests..."
	./$(PERFORMANCE_MONITORING_TEST)

$(PERFORMANCE_MONITORING_TEST): performance_monitoring_test.c $(PERFORMANCE_MONITORING_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Simple Performance Monitoring Test
SIMPLE_PERFORMANCE_TEST = $(BINDIR)/simple_performance_test
SIMPLE_PERFORMANCE_SOURCES = src/concurrency/performance_monitoring.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-simple-performance: $(SIMPLE_PERFORMANCE_TEST)
	@echo "Running simple performance monitoring tests..."
	./$(SIMPLE_PERFORMANCE_TEST)

$(SIMPLE_PERFORMANCE_TEST): simple_performance_test.c $(SIMPLE_PERFORMANCE_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Parallel Capability Security Test
PARALLEL_CAPABILITY_TEST = $(BINDIR)/parallel_capability_test
PARALLEL_CAPABILITY_SOURCES = src/concurrency/parallel_capability_security.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-parallel-capability: $(PARALLEL_CAPABILITY_TEST)
	@echo "Running parallel capability security tests..."
	./$(PARALLEL_CAPABILITY_TEST)

$(PARALLEL_CAPABILITY_TEST): parallel_capability_test.c $(PARALLEL_CAPABILITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Simple Capability Security Test
SIMPLE_CAPABILITY_TEST = $(BINDIR)/simple_capability_test
SIMPLE_CAPABILITY_SOURCES = src/concurrency/parallel_capability_security.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-simple-capability: $(SIMPLE_CAPABILITY_TEST)
	@echo "Running simple capability security tests..."
	./$(SIMPLE_CAPABILITY_TEST)

$(SIMPLE_CAPABILITY_TEST): simple_capability_test.c $(SIMPLE_CAPABILITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Minimal Capability Security Test
MINIMAL_CAPABILITY_TEST = $(BINDIR)/minimal_capability_test
MINIMAL_CAPABILITY_SOURCES = src/concurrency/parallel_capability_security.c src/errors/error.c

test-minimal-capability: $(MINIMAL_CAPABILITY_TEST)
	@echo "Running minimal capability security tests..."
	./$(MINIMAL_CAPABILITY_TEST)

$(MINIMAL_CAPABILITY_TEST): minimal_capability_test.c $(MINIMAL_CAPABILITY_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Capability-Only Test (no dependencies)
CAPABILITY_ONLY_TEST = $(BINDIR)/capability_only_test

test-capability-only: $(CAPABILITY_ONLY_TEST)
	@echo "Running capability-only security tests..."
	./$(CAPABILITY_ONLY_TEST)

$(CAPABILITY_ONLY_TEST): capability_only_test.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# NUMA Scheduling Test
NUMA_SCHEDULING_TEST = $(BINDIR)/numa_scheduling_test
NUMA_SCHEDULING_SOURCES = src/concurrency/numa_scheduling.c src/concurrency/performance_monitoring.c src/concurrency/structured_concurrency.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-numa-scheduling: $(NUMA_SCHEDULING_TEST)
	@echo "Running NUMA scheduling tests..."
	./$(NUMA_SCHEDULING_TEST)

$(NUMA_SCHEDULING_TEST): numa_scheduling_test.c $(NUMA_SCHEDULING_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Task 21.4 Advanced Channels Demo
TASK_21_4_DEMO = $(BINDIR)/task_21_4_advanced_channels_demo

test-task-21-4: $(TASK_21_4_DEMO)
	@echo "Running Task 21.4 Advanced Channel Patterns demo..."
	./$(TASK_21_4_DEMO)

$(TASK_21_4_DEMO): tests/examples/task_21_4_advanced_channels_demo.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Task 21.5 Deadlock Prevention Demo
TASK_21_5_DEMO = $(BINDIR)/task_21_5_deadlock_prevention_demo

test-task-21-5: $(TASK_21_5_DEMO)
	@echo "Running Task 21.5 Deadlock Prevention and Performance Optimization demo..."
	./$(TASK_21_5_DEMO)

$(TASK_21_5_DEMO): tests/examples/task_21_5_deadlock_prevention_demo.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Task 25.1 Async Runtime Demo
TASK_25_1_DEMO = $(BINDIR)/task_25_1_async_runtime_demo
ASYNC_RUNTIME_SOURCES = src/async/transparent_async.c src/errors/error.c

test-task-25-1: $(TASK_25_1_DEMO)
	@echo "Running Task 25.1 Core Async Runtime demo..."
	./$(TASK_25_1_DEMO)

$(TASK_25_1_DEMO): tests/examples/task_25_1_async_runtime_demo.c $(ASYNC_RUNTIME_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p src/async
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Transparent Async Test
TRANSPARENT_ASYNC_TEST = $(BINDIR)/transparent_async_test
TRANSPARENT_ASYNC_SOURCES = src/async/transparent_async.c src/async/transparent_execution.c src/errors/error.c src/errors/ergonomic_errors.c

test-transparent-async: $(TRANSPARENT_ASYNC_TEST)
	@echo "Running transparent async system tests..."
	./$(TRANSPARENT_ASYNC_TEST)

$(TRANSPARENT_ASYNC_TEST): tests/concurrency/transparent_async_test.c $(TRANSPARENT_ASYNC_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p src/async
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Enhanced Structured Concurrency Test
STRUCTURED_CONCURRENCY_ENHANCED_TEST = $(BINDIR)/structured_concurrency_enhanced_test
STRUCTURED_CONCURRENCY_ENHANCED_SOURCES = src/concurrency/structured_concurrency_enhanced.c src/concurrency/structured_concurrency.c src/async/transparent_async.c src/async/transparent_execution.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

test-structured-concurrency-enhanced: $(STRUCTURED_CONCURRENCY_ENHANCED_TEST)
	@echo "Running enhanced structured concurrency tests..."
	./$(STRUCTURED_CONCURRENCY_ENHANCED_TEST)

$(STRUCTURED_CONCURRENCY_ENHANCED_TEST): tests/concurrency/structured_concurrency_enhanced_test.c $(STRUCTURED_CONCURRENCY_ENHANCED_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p src/concurrency
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Enhanced Structured Concurrency Demo
STRUCTURED_CONCURRENCY_DEMO = $(BINDIR)/structured_concurrency_demo
STRUCTURED_CONCURRENCY_DEMO_SOURCES = src/concurrency/structured_concurrency_enhanced.c src/concurrency/structured_concurrency.c src/async/transparent_async.c src/async/transparent_execution.c src/errors/error.c src/errors/ergonomic_errors.c src/runtime/actor_system.c

demo-structured-concurrency: $(STRUCTURED_CONCURRENCY_DEMO)
	@echo "Running enhanced structured concurrency demo..."
	./$(STRUCTURED_CONCURRENCY_DEMO)

$(STRUCTURED_CONCURRENCY_DEMO): tests/examples/structured_concurrency_demo.c $(STRUCTURED_CONCURRENCY_DEMO_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p src/concurrency
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Async Resource Management Test
ASYNC_RESOURCE_TEST = $(BINDIR)/async_resource_test
ASYNC_RESOURCE_SOURCES = src/async/async_resource.c src/errors/error.c

test-async-resource: $(ASYNC_RESOURCE_TEST)
	@echo "Running async resource management tests..."
	./$(ASYNC_RESOURCE_TEST)

$(ASYNC_RESOURCE_TEST): tests/async/async_resource_test.c $(ASYNC_RESOURCE_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p tests/async
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Async Resource Management Demo
ASYNC_RESOURCE_DEMO = $(BINDIR)/async_resource_demo

demo-async-resource: $(ASYNC_RESOURCE_DEMO)
	@echo "Running async resource management demo..."
	./$(ASYNC_RESOURCE_DEMO)

$(ASYNC_RESOURCE_DEMO): examples/async_resource_demo.c $(ASYNC_RESOURCE_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Reactive Programming Test
REACTIVE_PROGRAMMING_TEST = $(BINDIR)/reactive_programming_test
REACTIVE_PROGRAMMING_SOURCES = src/async/reactive_programming_simple.c src/errors/error.c

test-reactive-programming: $(REACTIVE_PROGRAMMING_TEST)
	@echo "Running reactive programming tests..."
	./$(REACTIVE_PROGRAMMING_TEST)

$(REACTIVE_PROGRAMMING_TEST): tests/async/reactive_programming_minimal_test.c $(REACTIVE_PROGRAMMING_SOURCES)
	@mkdir -p $(BINDIR)
	@mkdir -p tests/async
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Reactive Programming Demo
REACTIVE_PROGRAMMING_DEMO = $(BINDIR)/reactive_programming_demo

demo-reactive-programming: $(REACTIVE_PROGRAMMING_DEMO)
	@echo "Running reactive programming demo..."
	./$(REACTIVE_PROGRAMMING_DEMO)

$(REACTIVE_PROGRAMMING_DEMO): examples/reactive_programming_demo.c $(REACTIVE_PROGRAMMING_SOURCES) $(ASYNC_RESOURCE_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile-time execution test
comptime_test: tests/test_comptime.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile-time types integration test
comptime_types_test: tests/test_comptime_types.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Optimization directives framework test
optimization_test: tests/test_optimization.c $(SRCDIR)/comptime/optimization.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Profile-Guided Optimization test
pgo_test: tests/test_pgo.c $(SRCDIR)/comptime/profile_guided_optimization.c $(SRCDIR)/comptime/optimization.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Advanced Optimization Strategies test
advanced_optimization_test: tests/test_advanced_optimization.c $(SRCDIR)/comptime/advanced_optimization.c $(SRCDIR)/comptime/profile_guided_optimization.c $(SRCDIR)/comptime/optimization.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

advanced_macro_test: tests/test_advanced_macro.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/types/types.c $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

derive_macro_test: tests/test_derive_macros.c $(SRCDIR)/derive_macros.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/types/types.c $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

template_macro_test: tests/test_template_macros.c $(SRCDIR)/template_macros.c $(SRCDIR)/derive_macros.c $(SRCDIR)/advanced_macro_system.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/types/types.c $(SRCDIR)/errors/error.c $(SRCDIR)/errors/ergonomic_errors.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
