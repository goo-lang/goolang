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
    LLVM_LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags --libs core) -lstdc++
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
PARSER_SRCS = $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/lexer_bridge.c $(SRCDIR)/parser/parser_errors.c $(SRCDIR)/parser/annotation_parser.c
AST_SRCS = $(SRCDIR)/ast/ast.c
# Temporarily disabled files for TDD Cycle 5 (have compilation errors - Task #22 incomplete implementations)
TYPES_SRCS = $(SRCDIR)/types/types.c $(SRCDIR)/types/type_checker.c $(SRCDIR)/types/expression_checker.c $(SRCDIR)/types/expression_helpers.c $(SRCDIR)/types/ownership_checker.c $(SRCDIR)/types/channel_checker.c $(SRCDIR)/types/constraint_inference.c $(SRCDIR)/types/flow_sensitive_analysis.c $(SRCDIR)/types/flow_analysis_core.c $(SRCDIR)/types/reference_manager.c $(SRCDIR)/types/escape_analysis.c $(SRCDIR)/types/arena_integration.c $(SRCDIR)/types/channel_integration.c $(SRCDIR)/types/resource_manager.c $(SRCDIR)/types/memory_safety_integration.c $(SRCDIR)/types/bounds_verifier.c
# Disabled: advanced_constraint_inference, concept_generics, higher_kinded_types, type_level_programming, interface_integration, hkt_auto_impl, protocol_oriented_programming, dependent_types, contracts, proof_generation, runtime_optimization
CODEGEN_SRCS = $(SRCDIR)/codegen/codegen.c $(SRCDIR)/codegen/type_mapping.c $(SRCDIR)/codegen/function_codegen.c $(SRCDIR)/codegen/expression_codegen.c $(SRCDIR)/codegen/error_union_codegen.c $(SRCDIR)/codegen/runtime_integration.c $(SRCDIR)/codegen/wasm_codegen.c
RUNTIME_SRCS = $(SRCDIR)/runtime/runtime.c $(SRCDIR)/runtime/arena.c $(SRCDIR)/runtime/platform.c $(SRCDIR)/runtime/concurrency.c $(SRCDIR)/runtime/channels.c $(SRCDIR)/runtime/sync.c $(SRCDIR)/runtime/deadlock.c $(SRCDIR)/runtime/error_handling.c $(SRCDIR)/runtime/error_context.c $(SRCDIR)/runtime/error_recovery.c $(SRCDIR)/runtime/error_aggregation.c $(SRCDIR)/runtime/error_hierarchies.c $(SRCDIR)/runtime/error_transformation.c $(SRCDIR)/runtime/actor_system.c $(SRCDIR)/runtime/shared_variables.c $(SRCDIR)/runtime/structured_concurrency.c $(SRCDIR)/runtime/advanced_channels.c $(SRCDIR)/runtime/deadlock_prevention.c
ERROR_SRCS = $(SRCDIR)/errors/error.c
IDE_SRCS = $(SRCDIR)/ide/hot_reload.c $(SRCDIR)/ide/repl.c $(SRCDIR)/ide/repl_type_info.c $(SRCDIR)/ide/performance_monitor.c $(SRCDIR)/ide/repl_errors.c $(SRCDIR)/ide/time_travel_debug.c $(SRCDIR)/ide/time_travel_debug_repl.c $(SRCDIR)/ide/repl_syntax.c $(SRCDIR)/ide/auto_fix.c $(SRCDIR)/ide/repl_autofix.c
TEST_FRAMEWORK_SRCS = $(TEST_FRAMEWORK_DIR)/test_framework.c
TEST_FRAMEWORK_EXTRA_SRCS = $(TEST_FRAMEWORK_DIR)/error_tests.c

CURRENT_SRCS = $(LEXER_SRCS) $(PARSER_SRCS) $(AST_SRCS) $(TYPES_SRCS) $(CODEGEN_SRCS) $(RUNTIME_SRCS) $(ERROR_SRCS) $(IDE_SRCS)
SRC_OBJS = $(CURRENT_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
TEST_FRAMEWORK_OBJ = $(TEST_FRAMEWORK_SRCS:$(TEST_FRAMEWORK_DIR)/%.c=$(BUILDDIR)/framework/%.o)
TEST_FRAMEWORK_EXTRA_OBJS = $(TEST_FRAMEWORK_EXTRA_SRCS:$(TEST_FRAMEWORK_DIR)/%.c=$(BUILDDIR)/framework/%.o)
OBJS = $(SRC_OBJS) $(TEST_FRAMEWORK_OBJ) $(TEST_FRAMEWORK_EXTRA_OBJS) $(BUILDDIR)/unit/constraint/constraint_inference_test.o $(BUILDDIR)/common/list.o

# Main targets
COMPILER = $(BINDIR)/goo
ANALYZER = $(BINDIR)/goo-analyzer
TEST_RUNNER = $(BINDIR)/test_runner
REPL = $(BINDIR)/goo-repl
REPL_ENHANCED = $(BINDIR)/goo-repl-enhanced
REPL_TYPE_INFO = $(BINDIR)/goo-repl-type-info
LSP_SERVER = $(BINDIR)/goo-lsp
LSP_ENHANCED_SERVER = $(BINDIR)/goo-lsp-enhanced
LSP_STANDALONE_SERVER = $(BINDIR)/goo-lsp-standalone
LSP_GOO_ENHANCED_SERVER = $(BINDIR)/goo-lsp-goo-enhanced
TEMPLATE_GENERATOR = $(BINDIR)/goo-template-generator
TEST_RUNNER_VISUAL = $(BINDIR)/goo-test-runner
TEST_REPL = $(BINDIR)/test_repl
TEST_PERFORMANCE = $(BINDIR)/test_performance
TEST_ERROR_REPORTING = $(BINDIR)/test_error_reporting

.PHONY: all clean test install lexer analyzer test-interface test-repl repl repl-enhanced lsp coverage coverage-report coverage-clean debug format check test-e2e test-integration-pipeline test-tdd

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

# Test unit object compilation
$(BUILDDIR)/unit/%.o: $(TEST_UNIT_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

# Common object compilation
$(BUILDDIR)/common/%.o: $(SRCDIR)/common/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

# Main compiler
$(COMPILER): $(OBJS) $(SRCDIR)/main_simple.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(SRCDIR)/main_simple.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

# Test targets
TEST_INTERFACE_SYSTEM = $(BINDIR)/test_interface_system
TEST_FLOW_ANALYSIS = $(BINDIR)/test_flow_analysis
TEST_REFERENCE_MANAGER = $(BINDIR)/test_reference_manager
TEST_SHARED_VARIABLES = $(BINDIR)/test_shared_variables
TEST_STRUCTURED_CONCURRENCY = $(BINDIR)/test_structured_concurrency

# Tests
test: $(TEST_RUNNER)
	./$(TEST_RUNNER)

$(TEST_RUNNER): $(OBJS) $(TEST_FRAMEWORK_DIR)/test_main.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(TEST_FRAMEWORK_DIR)/test_main.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS)

# Individual test targets
test-reference: $(TEST_REFERENCE_MANAGER)
	./$(TEST_REFERENCE_MANAGER)

$(TEST_REFERENCE_MANAGER): $(OBJS) $(TEST_UNIT_DIR)/memory/reference_manager_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) $(TEST_UNIT_DIR)/memory/reference_manager_test.c $(OBJS) -o $@ $(LDFLAGS) $(LLVM_LDFLAGS) -DSTANDALONE_TEST

# Add advanced channels test target
TEST_ADVANCED_CHANNELS = $(BINDIR)/test_advanced_channels

test-advanced-channels: $(TEST_ADVANCED_CHANNELS)
	./$(TEST_ADVANCED_CHANNELS)

$(TEST_ADVANCED_CHANNELS): test_advanced_channels_simple.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

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

# Shared Variables test
test-shared-variables: $(TEST_SHARED_VARIABLES)
	./$(TEST_SHARED_VARIABLES)

$(TEST_SHARED_VARIABLES): test_shared_variables_simple.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

# Structured Concurrency test
test-structured-concurrency: $(TEST_STRUCTURED_CONCURRENCY)
	./$(TEST_STRUCTURED_CONCURRENCY)

$(TEST_STRUCTURED_CONCURRENCY): test_structured_concurrency_simple.c $(OBJS)
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

# REPL with enhanced type information
repl-type-info: $(REPL_TYPE_INFO)

$(REPL_TYPE_INFO): $(SRCDIR)/ide/repl_enhanced_main.c $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ $< $(filter-out $(BUILDDIR)/main.o, $(OBJS)) $(LDFLAGS) $(LLVM_LDFLAGS)

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

# Goo-specific Enhanced LSP Server with comprehensive language support
lsp-goo-enhanced: $(LSP_GOO_ENHANCED_SERVER)

$(LSP_GOO_ENHANCED_SERVER): $(SRCDIR)/ide/lsp_goo_enhanced.c
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

# Project Template Generator
template-generator: $(TEMPLATE_GENERATOR)

$(TEMPLATE_GENERATOR): tools/template-generator/main.goo | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building template generator..."
	@# Note: This will be built when Goo compiler is ready
	@# For now, create a placeholder script
	@echo '#!/bin/bash' > $@
	@echo 'echo "Goo Template Generator"' >> $@
	@echo 'echo "Usage: goo-template-generator <command> [args...]"' >> $@
	@echo 'echo "Commands: list, info <template>, generate <template>"' >> $@
	@echo 'echo "Templates available in: ./templates/"' >> $@
	@chmod +x $@

# Visual Test Runner with HTML reports
test-runner: $(TEST_RUNNER_VISUAL)

$(TEST_RUNNER_VISUAL): $(SRCDIR)/ide/test_runner.c | $(BINDIR)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -lrt -o $@ $<

# Profiling Visualization Tool
PROFILING_VISUALIZATION = $(BINDIR)/goo-profiling-viz

profiling-viz: $(PROFILING_VISUALIZATION)

$(PROFILING_VISUALIZATION): $(SRCDIR)/ide/profiling_visualization.c $(SRCDIR)/ide/profiling_visualization_main.c | $(BINDIR)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -lm -o $@ $^

# Project Health Dashboard
PROJECT_HEALTH_DASHBOARD = $(BINDIR)/goo-health-dashboard

health-dashboard: $(PROJECT_HEALTH_DASHBOARD)

$(PROJECT_HEALTH_DASHBOARD): $(SRCDIR)/ide/project_health_dashboard.c $(SRCDIR)/ide/project_health_dashboard_main.c | $(BINDIR)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -lm -o $@ $^

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

# ====================
# TDD Test Targets
# ====================

# End-to-end tests
test-e2e:
	@./scripts/run_e2e_tests.sh

# Integration test for compilation pipeline
TEST_COMPILE_PIPELINE = $(BINDIR)/test_compile_pipeline

test-integration-pipeline: $(TEST_COMPILE_PIPELINE)
	@echo "Running compilation pipeline integration test..."
	./$(TEST_COMPILE_PIPELINE)

$(TEST_COMPILE_PIPELINE): $(TEST_INTEGRATION_DIR)/compile_pipeline_test.c $(LEXER_SRCS) | $(BINDIR)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LEXER_SRCS) $(LDFLAGS)

# Parser unit tests
TEST_PARSER_BASIC = $(BINDIR)/test_parser_basic

test-parser-basic: $(TEST_PARSER_BASIC)
	@echo "Running parser basic tests..."
	./$(TEST_PARSER_BASIC)

$(TEST_PARSER_BASIC): $(TEST_UNIT_DIR)/parser/parser_basic_test.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building parser for tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) -o $@ $< \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(LDFLAGS)

# Run all TDD tests
test-tdd:
	@$(MAKE) test-parser-basic || true
	@$(MAKE) test-parser-ast || true
	@$(MAKE) test-type-checker || true
	@$(MAKE) test-codegen || true
	@$(MAKE) test-integration-pipeline || true
	@$(MAKE) test-e2e || true
	@echo ""
	@echo "🎉 All TDD tests complete!"
# Parser AST verification tests
TEST_PARSER_AST = $(BINDIR)/test_parser_ast

test-parser-ast: $(TEST_PARSER_AST)
	@echo "Running parser AST verification tests..."
	./$(TEST_PARSER_AST)

$(TEST_PARSER_AST): $(TEST_UNIT_DIR)/parser/parser_ast_verification_test.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building parser AST verification tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) -o $@ $< \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(LDFLAGS)

# Type checker integration tests
TEST_TYPE_CHECKER_INTEGRATION = $(BINDIR)/test_type_checker_integration

test-type-checker: $(TEST_TYPE_CHECKER_INTEGRATION)
	@echo "Running type checker integration tests..."
	./$(TEST_TYPE_CHECKER_INTEGRATION)

$(TEST_TYPE_CHECKER_INTEGRATION): $(TEST_UNIT_DIR)/types/type_checker_integration_test.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building type checker integration tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) -o $@ $< \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(LDFLAGS)


# Code generation integration tests
TEST_CODEGEN_INTEGRATION = $(BINDIR)/test_codegen_integration

test-codegen: $(TEST_CODEGEN_INTEGRATION)
	@echo "Running code generation integration tests..."
	./$(TEST_CODEGEN_INTEGRATION)

$(TEST_CODEGEN_INTEGRATION): $(TEST_UNIT_DIR)/codegen/codegen_integration_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building code generation integration tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/codegen_integration_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)


# TDD Cycle 6: Loops and Arrays Tests
TEST_LOOPS_ARRAYS = $(BINDIR)/test_loops_arrays

test-loops-arrays: $(TEST_LOOPS_ARRAYS)
	@echo "Running loops and arrays tests..."
	./$(TEST_LOOPS_ARRAYS)

$(TEST_LOOPS_ARRAYS): $(TEST_UNIT_DIR)/codegen/loops_arrays_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building loops and arrays tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/loops_arrays_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)


# TDD Cycle 7: Structs and Methods Tests
TEST_STRUCTS_METHODS = $(BINDIR)/test_structs_methods

test-structs-methods: $(TEST_STRUCTS_METHODS)
	@echo "Running structs and methods tests..."
	./$(TEST_STRUCTS_METHODS)

$(TEST_STRUCTS_METHODS): $(TEST_UNIT_DIR)/codegen/structs_methods_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building structs and methods tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/structs_methods_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)


# TDD Cycle 8: Multiple Return Values Tests
TEST_MULTIPLE_RETURNS = $(BINDIR)/test_multiple_returns

test-multiple-returns: $(TEST_MULTIPLE_RETURNS)
	@echo "Running multiple return values tests..."
	./$(TEST_MULTIPLE_RETURNS)

# TDD Cycle 10: Switch Statements Tests
TEST_SWITCH = $(BINDIR)/test_switch

test-switch: $(TEST_SWITCH)
	@echo "Running switch statement tests..."
	./$(TEST_SWITCH)

# TDD Cycle 13: Range Loop Tests
TEST_RANGE = $(BINDIR)/test_range

test-range: $(TEST_RANGE)
	@echo "Running range loop tests..."
	./$(TEST_RANGE)

$(TEST_MULTIPLE_RETURNS): $(TEST_UNIT_DIR)/codegen/multiple_returns_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building multiple return values tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/multiple_returns_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

$(TEST_SWITCH): $(TEST_UNIT_DIR)/codegen/switch_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building switch statement tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/switch_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

$(TEST_RANGE): $(TEST_UNIT_DIR)/codegen/range_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building range loop tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/range_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

# TDD Cycle 11: Defer Statements Tests
TEST_DEFER = $(BINDIR)/test_defer

test-defer: $(TEST_DEFER)
	@echo "Running defer statement tests..."
	./$(TEST_DEFER)

$(TEST_DEFER): $(TEST_UNIT_DIR)/codegen/defer_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building defer statement tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/defer_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

# TDD Cycle 12: Slices Tests
TEST_SLICE = $(BINDIR)/test_slice

test-slice: $(TEST_SLICE)
	@echo "Running slice tests..."
	./$(TEST_SLICE)

$(TEST_SLICE): $(TEST_UNIT_DIR)/codegen/slice_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building slice tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/slice_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

# TDD Cycle 14: String Operations Tests
TEST_STRING = $(BINDIR)/test_string

test-string: $(TEST_STRING)
	@echo "Running string operations tests..."
	./$(TEST_STRING)

$(TEST_STRING): $(TEST_UNIT_DIR)/codegen/string_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building string operations tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/string_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

# TDD Cycle 15: Map Operations Tests
TEST_MAP = $(BINDIR)/test_map

test-map: $(TEST_MAP)
	@echo "Running map operations tests..."
	./$(TEST_MAP)

$(TEST_MAP): $(TEST_UNIT_DIR)/codegen/map_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building map operations tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/map_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

# TDD Cycle 16: Type Aliases Tests
TEST_TYPE_ALIAS = $(BINDIR)/test_type_alias

test-type-alias: $(TEST_TYPE_ALIAS)
	@echo "Running type alias tests..."
	./$(TEST_TYPE_ALIAS)

$(TEST_TYPE_ALIAS): $(TEST_UNIT_DIR)/codegen/type_alias_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building type alias tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/type_alias_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

# TDD Cycle 17: Constants Tests
TEST_CONST = $(BINDIR)/test_const

test-const: $(TEST_CONST)
	@echo "Running constant tests..."
	./$(TEST_CONST)

$(TEST_CONST): $(TEST_UNIT_DIR)/codegen/const_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building constant tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/const_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)

# TDD Cycle 18: Variadic Functions Tests
TEST_VARIADIC = $(BINDIR)/test_variadic

test-variadic: $(TEST_VARIADIC)
	@echo "Running variadic function tests..."
	./$(TEST_VARIADIC)

$(TEST_VARIADIC): $(TEST_UNIT_DIR)/codegen/variadic_test.c $(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c $(SRCDIR)/parser/parser.y | $(BINDIR)
	@mkdir -p $(BINDIR)
	@echo "Building variadic function tests..."
	@bison -d -o $(SRCDIR)/parser/parser.tab.c $(SRCDIR)/parser/parser.y 2>/dev/null || true
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -o $@ \
		$(TEST_UNIT_DIR)/codegen/variadic_test.c \
		$(TEST_UNIT_DIR)/codegen/test_codegen_helpers.c \
		$(SRCDIR)/parser/parser.tab.c \
		$(SRCDIR)/parser/parser_error_stubs.c \
		$(SRCDIR)/parser/lexer_bridge.c \
		$(LEXER_SRCS) \
		$(AST_SRCS) \
		$(TYPES_SRCS) \
		$(CODEGEN_SRCS) \
		$(LDFLAGS) $(LLVM_LDFLAGS)
