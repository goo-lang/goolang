#include "macros.h"
#include "comptime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while(0)
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))
#define EXPECT_EQ(a, b) do { \
    if ((a) != (b)) { printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); return 1; } \
} while(0)
#define EXPECT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { printf("  FAIL: \"%s\" != \"%s\" (line %d)\n", (a), (b), __LINE__); return 1; } \
} while(0)
#define EXPECT_NOT_NULL(p) do { \
    if ((p) == NULL) { printf("  FAIL: %s is NULL (line %d)\n", #p, __LINE__); return 1; } \
} while(0)

// =============================================================================
// Template Substitution Tests
// =============================================================================

static int test_template_substitute_basic(void) {
    char* result = template_substitute("Hello, {{name}}!", "name", "World");
    EXPECT_NOT_NULL(result);
    EXPECT_STR_EQ(result, "Hello, World!");
    free(result);
    return 0;
}

static int test_template_substitute_multiple(void) {
    char* result = template_substitute("{{x}} + {{x}} = 2*{{x}}", "x", "5");
    EXPECT_NOT_NULL(result);
    EXPECT_STR_EQ(result, "5 + 5 = 2*5");
    free(result);
    return 0;
}

static int test_template_substitute_no_match(void) {
    char* result = template_substitute("no placeholders here", "x", "5");
    EXPECT_NOT_NULL(result);
    EXPECT_STR_EQ(result, "no placeholders here");
    free(result);
    return 0;
}

static int test_template_substitute_code_gen(void) {
    char* tmpl = "func create_{{type}}(data: {{type}}) !{{type}} { return db.insert(data) }";
    char* result = template_substitute(tmpl, "type", "User");
    EXPECT_NOT_NULL(result);
    EXPECT_STR_EQ(result, "func create_User(data: User) !User { return db.insert(data) }");
    free(result);
    return 0;
}

// =============================================================================
// Filter Tests
// =============================================================================

static int test_filter_lowercase(void) {
    char* result = template_apply_filter("MyTypeName", "lowercase");
    EXPECT_NOT_NULL(result);
    EXPECT_STR_EQ(result, "mytypename");
    free(result);
    return 0;
}

static int test_filter_uppercase(void) {
    char* result = template_apply_filter("hello", "uppercase");
    EXPECT_NOT_NULL(result);
    EXPECT_STR_EQ(result, "HELLO");
    free(result);
    return 0;
}

static int test_filter_snake_case(void) {
    char* result = template_apply_filter("MyTypeName", "snake_case");
    EXPECT_NOT_NULL(result);
    EXPECT_STR_EQ(result, "my_type_name");
    free(result);
    return 0;
}

static int test_filter_identity(void) {
    char* result = template_apply_filter("hello", "");
    EXPECT_NOT_NULL(result);
    EXPECT_STR_EQ(result, "hello");
    free(result);
    return 0;
}

// =============================================================================
// Derive Parsing Tests
// =============================================================================

static int test_derive_parse_single(void) {
    DeriveSpec* spec = derive_parse_annotation("Debug");
    EXPECT_NOT_NULL(spec);
    EXPECT_EQ(spec->kind, DERIVE_DEBUG);
    EXPECT_TRUE(spec->next == NULL);
    derive_spec_free(spec);
    return 0;
}

static int test_derive_parse_multiple(void) {
    DeriveSpec* spec = derive_parse_annotation("Debug, Clone, Hash");
    EXPECT_NOT_NULL(spec);

    int count = 0;
    bool has_debug = false, has_clone = false, has_hash = false;
    for (DeriveSpec* s = spec; s; s = s->next) {
        if (s->kind == DERIVE_DEBUG) has_debug = true;
        if (s->kind == DERIVE_CLONE) has_clone = true;
        if (s->kind == DERIVE_HASH)  has_hash = true;
        count++;
    }

    EXPECT_EQ(count, 3);
    EXPECT_TRUE(has_debug);
    EXPECT_TRUE(has_clone);
    EXPECT_TRUE(has_hash);

    derive_spec_free(spec);
    return 0;
}

static int test_derive_parse_custom(void) {
    DeriveSpec* spec = derive_parse_annotation("MyCustomDerive");
    EXPECT_NOT_NULL(spec);
    EXPECT_EQ(spec->kind, DERIVE_CUSTOM);
    EXPECT_STR_EQ(spec->custom_name, "MyCustomDerive");
    derive_spec_free(spec);
    return 0;
}

// =============================================================================
// Macro System Tests
// =============================================================================

static int test_macro_system_lifecycle(void) {
    ComptimeInterpreter* comptime = comptime_interpreter_new();
    MacroSystem* ms = macro_system_new(comptime);
    EXPECT_NOT_NULL(ms);
    EXPECT_TRUE(ms->enforce_hygiene);
    EXPECT_EQ(ms->max_expansion_depth, (size_t)64);

    macro_system_free(ms);
    comptime_interpreter_free(comptime);
    return 0;
}

static int test_register_template(void) {
    ComptimeInterpreter* comptime = comptime_interpreter_new();
    MacroSystem* ms = macro_system_new(comptime);

    TemplateMacro* tmpl = template_macro_new("generate_getter",
        "func get_{{field}}(self: *{{type}}) {{field_type}} { return self.{{field}} }");
    EXPECT_NOT_NULL(tmpl);

    macro_system_register_template(ms, tmpl);
    EXPECT_EQ(ms->template_count, (size_t)1);

    macro_system_free(ms);
    comptime_interpreter_free(comptime);
    return 0;
}

static int test_expansion_lifecycle(void) {
    Position pos = {.line = 10, .column = 1, .filename = "test.goo"};
    MacroExpansion* exp = macro_expansion_new(pos, "derive");
    EXPECT_NOT_NULL(exp);
    EXPECT_EQ(exp->node_count, (size_t)0);

    macro_expansion_free(exp);
    return 0;
}

// =============================================================================
// Test Runner
// =============================================================================

typedef struct { const char* name; int (*func)(void); } TestCase;

int main(void) {
    TestCase tests[] = {
        {"template_substitute_basic",    test_template_substitute_basic},
        {"template_substitute_multiple", test_template_substitute_multiple},
        {"template_substitute_no_match", test_template_substitute_no_match},
        {"template_substitute_code_gen", test_template_substitute_code_gen},
        {"filter_lowercase",             test_filter_lowercase},
        {"filter_uppercase",             test_filter_uppercase},
        {"filter_snake_case",            test_filter_snake_case},
        {"filter_identity",              test_filter_identity},
        {"derive_parse_single",          test_derive_parse_single},
        {"derive_parse_multiple",        test_derive_parse_multiple},
        {"derive_parse_custom",          test_derive_parse_custom},
        {"macro_system_lifecycle",       test_macro_system_lifecycle},
        {"register_template",            test_register_template},
        {"expansion_lifecycle",          test_expansion_lifecycle},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Running %zu macro system tests...\n\n", total);

    for (size_t i = 0; i < total; i++) {
        printf("[ RUN      ] macros.%s\n", tests[i].name);
        int result = tests[i].func();
        if (result == 0) {
            printf("[\033[32m       OK\033[0m ] macros.%s\n", tests[i].name);
            passed++;
        } else {
            printf("[\033[31m  FAILED  \033[0m ] macros.%s\n", tests[i].name);
        }
    }

    printf("\nTests run: %zu\n\033[32mPassed: %zu\033[0m\n", total, passed);
    if (passed < total) printf("\033[31mFailed: %zu\033[0m\n", total - passed);

    return passed < total ? 1 : 0;
}
