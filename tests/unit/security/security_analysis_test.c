#include "security.h"
#include <stdio.h>
#include <string.h>

// Simple test assertion macros (non-fatal, report and continue)
static int g_test_failures = 0;

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); g_test_failures++; return 1; } \
} while(0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b) do { \
    if ((a) != (b)) { printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); g_test_failures++; return 1; } \
} while(0)

#define EXPECT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { printf("  FAIL: \"%s\" != \"%s\" (line %d)\n", (a), (b), __LINE__); g_test_failures++; return 1; } \
} while(0)

// =============================================================================
// Taint Info Tests
// =============================================================================

static int test_taint_info_none(void) {
    TaintInfo info = taint_info_none();
    EXPECT_FALSE(info.is_tainted);
    EXPECT_TRUE(taint_info_is_clean(&info));
    EXPECT_EQ(info.sources, TAINT_SOURCE_NONE);
    return 0;
}

static int test_taint_info_tainted(void) {
    Position origin = {.line = 10, .column = 5, .filename = "input.goo"};
    TaintInfo info = taint_info_tainted(TAINT_SOURCE_USER_INPUT, origin);

    EXPECT_TRUE(info.is_tainted);
    EXPECT_FALSE(taint_info_is_clean(&info));
    EXPECT_EQ(info.sources, (uint32_t)TAINT_SOURCE_USER_INPUT);
    EXPECT_EQ(info.taint_origin.line, 10);
    return 0;
}

static int test_taint_info_sanitized(void) {
    Position origin = {.line = 10, .column = 5, .filename = "input.goo"};
    Position sanitize = {.line = 20, .column = 3, .filename = "input.goo"};
    TaintInfo tainted = taint_info_tainted(TAINT_SOURCE_NETWORK, origin);
    TaintInfo sanitized = taint_info_sanitized(tainted, "html_escape", sanitize);

    EXPECT_FALSE(sanitized.is_tainted);
    EXPECT_TRUE(sanitized.is_sanitized);
    EXPECT_TRUE(taint_info_is_clean(&sanitized));
    EXPECT_STR_EQ(sanitized.sanitizer_name, "html_escape");
    EXPECT_EQ(sanitized.sanitize_point.line, 20);
    // Original source preserved for audit trail
    EXPECT_EQ(sanitized.sources, (uint32_t)TAINT_SOURCE_NETWORK);
    return 0;
}

// =============================================================================
// Taint Propagation Tests
// =============================================================================

static int test_propagate_binary_clean(void) {
    TaintInfo left = taint_info_none();
    TaintInfo right = taint_info_none();
    TaintInfo result = taint_propagate_binary(left, right);

    EXPECT_FALSE(result.is_tainted);
    return 0;
}

static int test_propagate_binary_left_tainted(void) {
    Position origin = {.line = 5, .column = 1, .filename = "test.goo"};
    TaintInfo left = taint_info_tainted(TAINT_SOURCE_USER_INPUT, origin);
    TaintInfo right = taint_info_none();
    TaintInfo result = taint_propagate_binary(left, right);

    EXPECT_TRUE(result.is_tainted);
    EXPECT_EQ(result.sources, (uint32_t)TAINT_SOURCE_USER_INPUT);
    return 0;
}

static int test_propagate_binary_both_tainted(void) {
    Position o1 = {.line = 5, .column = 1, .filename = "test.goo"};
    Position o2 = {.line = 8, .column = 1, .filename = "test.goo"};
    TaintInfo left = taint_info_tainted(TAINT_SOURCE_USER_INPUT, o1);
    TaintInfo right = taint_info_tainted(TAINT_SOURCE_NETWORK, o2);
    TaintInfo result = taint_propagate_binary(left, right);

    EXPECT_TRUE(result.is_tainted);
    // Sources should be merged
    EXPECT_TRUE(result.sources & TAINT_SOURCE_USER_INPUT);
    EXPECT_TRUE(result.sources & TAINT_SOURCE_NETWORK);
    return 0;
}

static int test_propagate_call_sanitizer(void) {
    Position origin = {.line = 5, .column = 1, .filename = "test.goo"};
    TaintInfo args[1] = {taint_info_tainted(TAINT_SOURCE_USER_INPUT, origin)};

    SecurityContext ctx = {0};
    ctx.is_sanitizer = true;
    ctx.sanitizer_name = "sql_escape";

    TaintInfo result = taint_propagate_call(args, 1, &ctx);

    EXPECT_FALSE(result.is_tainted);
    EXPECT_TRUE(result.is_sanitized);
    EXPECT_STR_EQ(result.sanitizer_name, "sql_escape");
    return 0;
}

static int test_propagate_call_trusted(void) {
    Position origin = {.line = 5, .column = 1, .filename = "test.goo"};
    TaintInfo args[1] = {taint_info_tainted(TAINT_SOURCE_NETWORK, origin)};

    SecurityContext ctx = {0};
    ctx.is_trusted = true;

    TaintInfo result = taint_propagate_call(args, 1, &ctx);

    EXPECT_FALSE(result.is_tainted);
    return 0;
}

static int test_propagate_call_normal(void) {
    Position origin = {.line = 5, .column = 1, .filename = "test.goo"};
    TaintInfo args[2] = {
        taint_info_none(),
        taint_info_tainted(TAINT_SOURCE_FILE, origin),
    };

    SecurityContext ctx = {0};
    TaintInfo result = taint_propagate_call(args, 2, &ctx);

    EXPECT_TRUE(result.is_tainted);
    EXPECT_EQ(result.sources, (uint32_t)TAINT_SOURCE_FILE);
    return 0;
}

// =============================================================================
// Security Sink Checking Tests
// =============================================================================

static int test_check_sink_clean_passes(void) {
    SecurityAnalyzer* analyzer = security_analyzer_new();
    TaintInfo clean = taint_info_none();
    Position pos = {.line = 10, .column = 1, .filename = "test.goo"};

    security_check_sink(analyzer, clean, SINK_SQL_QUERY, pos);

    EXPECT_EQ(analyzer->finding_count, (size_t)0);
    security_analyzer_free(analyzer);
    return 0;
}

static int test_check_sink_tainted_reports(void) {
    SecurityAnalyzer* analyzer = security_analyzer_new();
    Position origin = {.line = 5, .column = 1, .filename = "test.goo"};
    Position sink_pos = {.line = 20, .column = 5, .filename = "test.goo"};
    TaintInfo tainted = taint_info_tainted(TAINT_SOURCE_USER_INPUT, origin);

    security_check_sink(analyzer, tainted, SINK_SQL_QUERY, sink_pos);

    EXPECT_EQ(analyzer->finding_count, (size_t)1);
    EXPECT_EQ(analyzer->stats.taint_flows_detected, (size_t)1);

    SecurityFinding* f = analyzer->findings;
    EXPECT_EQ(f->kind, FINDING_TAINT_FLOW);
    EXPECT_TRUE(strstr(f->message, "user input") != NULL);
    EXPECT_TRUE(strstr(f->message, "SQL query") != NULL);

    security_analyzer_free(analyzer);
    return 0;
}

static int test_check_sink_strict_mode(void) {
    SecurityAnalyzer* analyzer = security_analyzer_new();
    security_analyzer_set_strict(analyzer, true);

    Position origin = {.line = 5, .column = 1, .filename = "test.goo"};
    Position sink_pos = {.line = 20, .column = 5, .filename = "test.goo"};
    TaintInfo tainted = taint_info_tainted(TAINT_SOURCE_NETWORK, origin);

    security_check_sink(analyzer, tainted, SINK_SHELL_COMMAND, sink_pos);

    EXPECT_TRUE(security_has_errors(analyzer));

    security_analyzer_free(analyzer);
    return 0;
}

// =============================================================================
// Capability Checking Tests
// =============================================================================

static int test_capability_satisfied(void) {
    SecurityAnalyzer* analyzer = security_analyzer_new();

    Capability required = {.kind = CAP_FILE_READ, .restriction = "/tmp/"};
    SecurityContext callee = {
        .required_caps = &required,
        .required_cap_count = 1,
    };

    Capability granted = {.kind = CAP_FILE_READ, .restriction = "/tmp/"};
    SecurityContext caller = {
        .granted_caps = &granted,
        .granted_cap_count = 1,
    };

    Position pos = {.line = 10, .column = 1, .filename = "test.goo"};
    security_check_capability(analyzer, &caller, &callee, pos);

    EXPECT_EQ(analyzer->finding_count, (size_t)0);
    security_analyzer_free(analyzer);
    return 0;
}

static int test_capability_missing(void) {
    SecurityAnalyzer* analyzer = security_analyzer_new();

    Capability required = {.kind = CAP_NETWORK_CONNECT, .restriction = NULL};
    SecurityContext callee = {
        .required_caps = &required,
        .required_cap_count = 1,
    };

    SecurityContext caller = {
        .granted_caps = NULL,
        .granted_cap_count = 0,
    };

    Position pos = {.line = 10, .column = 1, .filename = "test.goo"};
    security_check_capability(analyzer, &caller, &callee, pos);

    EXPECT_EQ(analyzer->finding_count, (size_t)1);
    EXPECT_TRUE(security_has_errors(analyzer));

    security_analyzer_free(analyzer);
    return 0;
}

// =============================================================================
// Annotation Parsing Tests
// =============================================================================

static int test_parse_tainted_user_input(void) {
    TaintSource source;
    EXPECT_TRUE(security_parse_tainted_annotation("user_input", &source));
    EXPECT_EQ(source, TAINT_SOURCE_USER_INPUT);
    return 0;
}

static int test_parse_tainted_network(void) {
    TaintSource source;
    EXPECT_TRUE(security_parse_tainted_annotation("network", &source));
    EXPECT_EQ(source, TAINT_SOURCE_NETWORK);
    return 0;
}

static int test_parse_tainted_default(void) {
    TaintSource source;
    EXPECT_TRUE(security_parse_tainted_annotation("", &source));
    EXPECT_EQ(source, TAINT_SOURCE_EXTERNAL);
    return 0;
}

static int test_parse_capability_file_read(void) {
    Capability cap;
    EXPECT_TRUE(security_parse_capability_annotation("filesystem.read=/tmp/", &cap));
    EXPECT_EQ(cap.kind, CAP_FILE_READ);
    EXPECT_STR_EQ(cap.restriction, "/tmp/");
    return 0;
}

static int test_parse_capability_network(void) {
    Capability cap;
    EXPECT_TRUE(security_parse_capability_annotation("network.connect", &cap));
    EXPECT_EQ(cap.kind, CAP_NETWORK_CONNECT);
    return 0;
}

static int test_parse_capability_invalid(void) {
    Capability cap;
    EXPECT_FALSE(security_parse_capability_annotation("invalid.thing", &cap));
    return 0;
}

// =============================================================================
// Test Runner
// =============================================================================

typedef struct {
    const char* name;
    int (*func)(void);
} SecurityTestCase;

int main(void) {
    SecurityTestCase tests[] = {
        // Taint info
        {"taint_info_none",             test_taint_info_none},
        {"taint_info_tainted",          test_taint_info_tainted},
        {"taint_info_sanitized",        test_taint_info_sanitized},
        // Taint propagation
        {"propagate_binary_clean",      test_propagate_binary_clean},
        {"propagate_binary_left_tainted", test_propagate_binary_left_tainted},
        {"propagate_binary_both_tainted", test_propagate_binary_both_tainted},
        {"propagate_call_sanitizer",    test_propagate_call_sanitizer},
        {"propagate_call_trusted",      test_propagate_call_trusted},
        {"propagate_call_normal",       test_propagate_call_normal},
        // Sink checking
        {"check_sink_clean_passes",     test_check_sink_clean_passes},
        {"check_sink_tainted_reports",  test_check_sink_tainted_reports},
        {"check_sink_strict_mode",      test_check_sink_strict_mode},
        // Capability checking
        {"capability_satisfied",        test_capability_satisfied},
        {"capability_missing",          test_capability_missing},
        // Annotation parsing
        {"parse_tainted_user_input",    test_parse_tainted_user_input},
        {"parse_tainted_network",       test_parse_tainted_network},
        {"parse_tainted_default",       test_parse_tainted_default},
        {"parse_capability_file_read",  test_parse_capability_file_read},
        {"parse_capability_network",    test_parse_capability_network},
        {"parse_capability_invalid",    test_parse_capability_invalid},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;
    size_t failed = 0;

    printf("Running %zu security analysis tests...\n\n", total);

    for (size_t i = 0; i < total; i++) {
        printf("[ RUN      ] security.%s\n", tests[i].name);
        int result = tests[i].func();
        if (result == 0) {
            printf("[\033[32m       OK\033[0m ] security.%s\n", tests[i].name);
            passed++;
        } else {
            printf("[\033[31m  FAILED  \033[0m ] security.%s\n", tests[i].name);
            failed++;
        }
    }

    printf("\nTests run: %zu\n", total);
    printf("\033[32mPassed: %zu\033[0m\n", passed);
    if (failed > 0) {
        printf("\033[31mFailed: %zu\033[0m\n", failed);
    }

    return failed > 0 ? 1 : 0;
}
