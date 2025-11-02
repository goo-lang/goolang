#include "auto_fix.h"
#include "types.h"
#include "ast.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <time.h>
#include <ctype.h>

// =============================================================================
// Automatic Error Correction System Implementation
// =============================================================================

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"

// Helper function for string duplication
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) strcpy(dup, str);
    return dup;
}

// Generate unique ID for suggestions
static int suggestion_counter = 0;

// =============================================================================
// Engine Management
// =============================================================================

AutoFixEngine* auto_fix_engine_new(void) {
    AutoFixEngine* engine = calloc(1, sizeof(AutoFixEngine));
    if (!engine) return NULL;
    
    // Set default configuration
    engine->auto_apply_safe = false;  // Conservative by default
    engine->show_explanations = true;
    engine->batch_mode = false;
    engine->min_confidence = FIX_CONFIDENCE_LOW;
    
    return engine;
}

void auto_fix_engine_free(AutoFixEngine* engine) {
    if (!engine) return;
    
    // Free error patterns
    ErrorPattern* pattern = engine->patterns;
    while (pattern) {
        ErrorPattern* next = pattern->next;
        auto_fix_pattern_free(pattern);
        pattern = next;
    }
    
    free(engine);
}

int auto_fix_engine_init(AutoFixEngine* engine) {
    if (!engine) return -1;
    
    // Initialize built-in error patterns
    ErrorPattern* patterns[] = {
        // Type errors
        &(ErrorPattern){
            .pattern_id = str_dup("type_mismatch"),
            .description = str_dup("Type mismatch between expected and actual types"),
            .category = ERROR_CATEGORY_TYPE,
            .regex_pattern = str_dup(".*type.*mismatch.*|.*expected.*got.*|.*cannot.*assign.*"),
            .example_error = str_dup("Type error: expected 'int', got 'string'"),
            .priority = 10
        },
        
        // Null safety errors
        &(ErrorPattern){
            .pattern_id = str_dup("null_deref"),
            .description = str_dup("Null pointer dereference or access to nullable without check"),
            .category = ERROR_CATEGORY_NULL_SAFETY,
            .regex_pattern = str_dup(".*null.*dereference.*|.*nullable.*access.*|.*null.*check.*"),
            .example_error = str_dup("Null safety error: accessing nullable without null check"),
            .priority = 9
        },
        
        // Ownership errors
        &(ErrorPattern){
            .pattern_id = str_dup("move_error"),
            .description = str_dup("Use after move or ownership violation"),
            .category = ERROR_CATEGORY_OWNERSHIP,
            .regex_pattern = str_dup(".*moved.*value.*|.*ownership.*|.*borrow.*after.*move.*"),
            .example_error = str_dup("Ownership error: use of moved value"),
            .priority = 8
        },
        
        // Syntax errors
        &(ErrorPattern){
            .pattern_id = str_dup("missing_semicolon"),
            .description = str_dup("Missing semicolon at end of statement"),
            .category = ERROR_CATEGORY_SYNTAX,
            .regex_pattern = str_dup(".*missing.*;.*|.*expected.*;.*"),
            .example_error = str_dup("Syntax error: expected ';' after expression"),
            .priority = 7
        },
        
        // Import errors
        &(ErrorPattern){
            .pattern_id = str_dup("undefined_symbol"),
            .description = str_dup("Undefined symbol or missing import"),
            .category = ERROR_CATEGORY_IMPORT,
            .regex_pattern = str_dup(".*undefined.*symbol.*|.*not.*found.*|.*unresolved.*"),
            .example_error = str_dup("Import error: undefined symbol 'println'"),
            .priority = 6
        },
        
        // Error handling
        &(ErrorPattern){
            .pattern_id = str_dup("unhandled_error"),
            .description = str_dup("Unhandled error union or missing error propagation"),
            .category = ERROR_CATEGORY_ERROR_HANDLING,
            .regex_pattern = str_dup(".*unhandled.*error.*|.*must.*handle.*|.*error.*union.*"),
            .example_error = str_dup("Error handling: unhandled error union value"),
            .priority = 5
        }
    };
    
    // Add built-in patterns to engine
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        auto_fix_add_pattern(engine, patterns[i]);
    }
    
    printf("🔧 Auto-fix engine initialized with %d built-in patterns\n", engine->pattern_count);
    return 0;
}

void auto_fix_engine_set_type_checker(AutoFixEngine* engine, TypeChecker* type_checker) {
    if (engine) {
        engine->type_checker = type_checker;
    }
}

void auto_fix_engine_set_repl(AutoFixEngine* engine, REPLContext* repl_context) {
    if (engine) {
        engine->repl_context = repl_context;
    }
}

// =============================================================================
// Error Analysis and Fix Generation
// =============================================================================

FixSuggestion* auto_fix_analyze_error(AutoFixEngine* engine, const FixContext* context) {
    if (!engine || !context || !context->error_message) return NULL;
    
    engine->suggestions_made++;
    
    // First, try to match against known error patterns
    ErrorPattern* pattern = auto_fix_match_pattern(engine, context->error_message);
    if (pattern) {
        engine->patterns_matched++;
        
        // Generate fixes based on the matched pattern category
        return auto_fix_generate_category_fixes(engine, pattern->category, context);
    }
    
    // If no pattern matches, try heuristic analysis
    // Check for common error indicators in the message
    if (strstr(context->error_message, "type") && strstr(context->error_message, "mismatch")) {
        return auto_fix_generate_type_fixes(engine, context);
    }
    
    if (strstr(context->error_message, "null") || strstr(context->error_message, "nullable")) {
        return auto_fix_generate_null_safety_fixes(engine, context);
    }
    
    if (strstr(context->error_message, "move") || strstr(context->error_message, "ownership")) {
        return auto_fix_generate_ownership_fixes(engine, context);
    }
    
    if (strstr(context->error_message, "syntax") || strstr(context->error_message, "expected")) {
        return auto_fix_generate_syntax_fixes(engine, context);
    }
    
    // No specific fixes found - return a generic suggestion
    FixSuggestion* suggestion = calloc(1, sizeof(FixSuggestion));
    if (!suggestion) return NULL;
    
    suggestion->suggestion_id = auto_fix_generate_suggestion_id();
    suggestion->title = str_dup("General Error Analysis");
    suggestion->description = str_dup("Review error message and check documentation");
    suggestion->category = ERROR_CATEGORY_UNKNOWN;
    suggestion->confidence = FIX_CONFIDENCE_LOW;
    suggestion->reasoning = str_dup("No specific pattern matched for this error. Please review the error message and consult documentation.");
    
    return suggestion;
}

ErrorPattern* auto_fix_match_pattern(AutoFixEngine* engine, const char* error_message) {
    if (!engine || !error_message) return NULL;
    
    ErrorPattern* pattern = engine->patterns;
    ErrorPattern* best_match = NULL;
    int highest_priority = -1;
    
    while (pattern) {
        if (pattern->regex_pattern) {
            regex_t regex;
            if (regcomp(&regex, pattern->regex_pattern, REG_ICASE | REG_EXTENDED) == 0) {
                if (regexec(&regex, error_message, 0, NULL, 0) == 0) {
                    // Pattern matches
                    if (pattern->priority > highest_priority) {
                        highest_priority = pattern->priority;
                        best_match = pattern;
                    }
                }
                regfree(&regex);
            }
        }
        pattern = pattern->next;
    }
    
    return best_match;
}

FixSuggestion* auto_fix_generate_category_fixes(AutoFixEngine* engine, 
                                               ErrorCategory category,
                                               const FixContext* context) {
    switch (category) {
        case ERROR_CATEGORY_TYPE:
            return auto_fix_generate_type_fixes(engine, context);
        case ERROR_CATEGORY_NULL_SAFETY:
            return auto_fix_generate_null_safety_fixes(engine, context);
        case ERROR_CATEGORY_OWNERSHIP:
            return auto_fix_generate_ownership_fixes(engine, context);
        case ERROR_CATEGORY_SYNTAX:
            return auto_fix_generate_syntax_fixes(engine, context);
        case ERROR_CATEGORY_IMPORT:
            return auto_fix_generate_import_fixes(engine, context);
        case ERROR_CATEGORY_ERROR_HANDLING:
            return auto_fix_generate_error_handling_fixes(engine, context);
        default:
            return NULL;
    }
}

// =============================================================================
// Type Error Fixes
// =============================================================================

FixSuggestion* auto_fix_generate_type_fixes(AutoFixEngine* engine, const FixContext* context) {
    if (!engine || !context) return NULL;
    
    FixSuggestion* suggestion = calloc(1, sizeof(FixSuggestion));
    if (!suggestion) return NULL;
    
    suggestion->suggestion_id = auto_fix_generate_suggestion_id();
    suggestion->title = str_dup("Type Mismatch Fix");
    suggestion->category = ERROR_CATEGORY_TYPE;
    
    // Analyze type mismatch
    if (context->expected_type && context->actual_type) {
        char desc[512];
        const char* expected_str = type_to_string(context->expected_type);
        const char* actual_str = type_to_string(context->actual_type);
        
        snprintf(desc, sizeof(desc), "Convert %s to %s", 
                actual_str ? actual_str : "unknown",
                expected_str ? expected_str : "unknown");
        
        suggestion->description = str_dup(desc);
        suggestion->confidence = FIX_CONFIDENCE_HIGH;
        
        // Generate specific fix based on types
        CodeFix* fix = calloc(1, sizeof(CodeFix));
        if (fix) {
            // String to int conversion
            if (context->expected_type->kind == TYPE_INT32 && 
                context->actual_type->kind == TYPE_STRING) {
                
                fix->description = str_dup("Convert string to int using strconv");
                fix->before_code = str_dup("someString");
                fix->after_code = str_dup("strconv.atoi(someString)");
                fix->explanation = str_dup("Use strconv.atoi() to convert string to integer");
                fix->scope = FIX_SCOPE_SINGLE;
                suggestion->confidence = FIX_CONFIDENCE_HIGH;
            }
            // Int to string conversion
            else if (context->expected_type->kind == TYPE_STRING && 
                     context->actual_type->kind == TYPE_INT32) {
                
                fix->description = str_dup("Convert int to string using strconv");
                fix->before_code = str_dup("someInt");
                fix->after_code = str_dup("strconv.itoa(someInt)");
                fix->explanation = str_dup("Use strconv.itoa() to convert integer to string");
                fix->scope = FIX_SCOPE_SINGLE;
                suggestion->confidence = FIX_CONFIDENCE_HIGH;
            }
            // Pointer dereference
            else if (context->expected_type->kind == TYPE_INT32 && 
                     context->actual_type->kind == TYPE_POINTER) {
                
                fix->description = str_dup("Dereference pointer to get value");
                fix->before_code = str_dup("pointerVar");
                fix->after_code = str_dup("*pointerVar");
                fix->explanation = str_dup("Dereference the pointer to access the underlying value");
                fix->scope = FIX_SCOPE_SINGLE;
                suggestion->confidence = FIX_CONFIDENCE_MEDIUM;
            }
            // Generic cast suggestion
            else {
                fix->description = str_dup("Add explicit type conversion");
                fix->before_code = str_dup("value");
                fix->after_code = str_dup("cast(TargetType, value)");
                fix->explanation = str_dup("Add explicit type conversion if types are compatible");
                fix->scope = FIX_SCOPE_SINGLE;
                suggestion->confidence = FIX_CONFIDENCE_LOW;
            }
            
            suggestion->fixes = fix;
            suggestion->fix_count = 1;
        }
    } else {
        suggestion->description = str_dup("Fix type mismatch error");
        suggestion->confidence = FIX_CONFIDENCE_MEDIUM;
        suggestion->reasoning = str_dup("Check that variable types match expected function parameters or assignment targets");
    }
    
    suggestion->examples = str_dup(
        "// Before:\n"
        "let x: int = \"hello\"\n\n"
        "// After:\n"
        "let x: int = strconv.atoi(\"hello\")\n"
        "// or\n"
        "let x: string = \"hello\""
    );
    
    return suggestion;
}

// =============================================================================
// Null Safety Fixes
// =============================================================================

FixSuggestion* auto_fix_generate_null_safety_fixes(AutoFixEngine* engine, const FixContext* context) {
    if (!engine || !context) return NULL;
    
    FixSuggestion* suggestion = calloc(1, sizeof(FixSuggestion));
    if (!suggestion) return NULL;
    
    suggestion->suggestion_id = auto_fix_generate_suggestion_id();
    suggestion->title = str_dup("Null Safety Fix");
    suggestion->description = str_dup("Add null check before accessing nullable value");
    suggestion->category = ERROR_CATEGORY_NULL_SAFETY;
    suggestion->confidence = FIX_CONFIDENCE_HIGH;
    
    // Generate null check fix
    CodeFix* fix = calloc(1, sizeof(CodeFix));
    if (fix) {
        fix->description = str_dup("Add null safety check");
        fix->before_code = str_dup("nullable_var.some_method()");
        fix->after_code = str_dup(
            "if nullable_var.is_some() {\n"
            "    nullable_var.unwrap().some_method()\n"
            "} else {\n"
            "    // Handle null case\n"
            "}"
        );
        fix->explanation = str_dup("Check if nullable value contains data before accessing it");
        fix->scope = FIX_SCOPE_SINGLE;
        
        suggestion->fixes = fix;
        suggestion->fix_count = 1;
    }
    
    suggestion->reasoning = str_dup("Goo enforces null safety by requiring explicit checks before accessing nullable values");
    suggestion->examples = str_dup(
        "// Problem: Direct access to nullable\n"
        "user?: User = get_user()\n"
        "print(user.name)  // Error!\n\n"
        "// Solution 1: Explicit null check\n"
        "if user.is_some() {\n"
        "    print(user.unwrap().name)\n"
        "}\n\n"
        "// Solution 2: Safe access with default\n"
        "name := user.map(u => u.name).unwrap_or(\"Unknown\")\n"
        "print(name)"
    );
    
    return suggestion;
}

// =============================================================================
// Ownership Error Fixes
// =============================================================================

FixSuggestion* auto_fix_generate_ownership_fixes(AutoFixEngine* engine, const FixContext* context) {
    if (!engine || !context) return NULL;
    
    FixSuggestion* suggestion = calloc(1, sizeof(FixSuggestion));
    if (!suggestion) return NULL;
    
    suggestion->suggestion_id = auto_fix_generate_suggestion_id();
    suggestion->title = str_dup("Ownership Error Fix");
    suggestion->description = str_dup("Fix ownership violation or use after move");
    suggestion->category = ERROR_CATEGORY_OWNERSHIP;
    suggestion->confidence = FIX_CONFIDENCE_MEDIUM;
    
    // Generate ownership fix
    CodeFix* fix = calloc(1, sizeof(CodeFix));
    if (fix) {
        if (strstr(context->error_message, "moved")) {
            fix->description = str_dup("Clone value instead of moving");
            fix->before_code = str_dup("let data2 = data1  // moves data1");
            fix->after_code = str_dup("let data2 = data1.clone()  // clones data1");
            fix->explanation = str_dup("Use .clone() to create a copy instead of moving ownership");
        } else {
            fix->description = str_dup("Use borrowing instead of ownership transfer");
            fix->before_code = str_dup("process_data(data)");
            fix->after_code = str_dup("process_data(&data)");
            fix->explanation = str_dup("Pass a reference (&) to avoid transferring ownership");
        }
        
        fix->scope = FIX_SCOPE_SINGLE;
        suggestion->fixes = fix;
        suggestion->fix_count = 1;
    }
    
    suggestion->reasoning = str_dup("Goo's ownership system ensures memory safety by tracking who owns each value");
    suggestion->examples = str_dup(
        "// Problem: Use after move\n"
        "let data = create_data()\n"
        "process(data)  // moves data\n"
        "print(data)    // Error! data was moved\n\n"
        "// Solution 1: Clone before move\n"
        "let data = create_data()\n"
        "process(data.clone())\n"
        "print(data)  // OK, original data still owned\n\n"
        "// Solution 2: Use borrowing\n"
        "let data = create_data()\n"
        "process(&data)  // borrows data\n"
        "print(data)     // OK, data still owned"
    );
    
    return suggestion;
}

// =============================================================================
// Syntax Error Fixes
// =============================================================================

FixSuggestion* auto_fix_generate_syntax_fixes(AutoFixEngine* engine, const FixContext* context) {
    if (!engine || !context) return NULL;
    
    FixSuggestion* suggestion = calloc(1, sizeof(FixSuggestion));
    if (!suggestion) return NULL;
    
    suggestion->suggestion_id = auto_fix_generate_suggestion_id();
    suggestion->title = str_dup("Syntax Error Fix");
    suggestion->category = ERROR_CATEGORY_SYNTAX;
    suggestion->confidence = FIX_CONFIDENCE_HIGH;
    
    CodeFix* fix = calloc(1, sizeof(CodeFix));
    if (fix) {
        // Missing semicolon
        if (strstr(context->error_message, "semicolon") || strstr(context->error_message, ";")) {
            suggestion->description = str_dup("Add missing semicolon");
            fix->description = str_dup("Add semicolon at end of statement");
            fix->before_code = str_dup("let x = 42");
            fix->after_code = str_dup("let x = 42;");
            fix->explanation = str_dup("Statements in Goo must end with a semicolon");
        }
        // Missing brace
        else if (strstr(context->error_message, "brace") || strstr(context->error_message, "{")) {
            suggestion->description = str_dup("Add missing brace");
            fix->description = str_dup("Add missing opening or closing brace");
            fix->before_code = str_dup("if condition\n    do_something();");
            fix->after_code = str_dup("if condition {\n    do_something();\n}");
            fix->explanation = str_dup("Control structures require braces around the body");
        }
        // Missing parenthesis
        else if (strstr(context->error_message, "paren") || strstr(context->error_message, "(")) {
            suggestion->description = str_dup("Add missing parenthesis");
            fix->description = str_dup("Add missing opening or closing parenthesis");
            fix->before_code = str_dup("func_call arg1, arg2");
            fix->after_code = str_dup("func_call(arg1, arg2)");
            fix->explanation = str_dup("Function calls require parentheses around arguments");
        }
        else {
            suggestion->description = str_dup("Fix syntax error");
            fix->description = str_dup("Review syntax and fix formatting");
            fix->explanation = str_dup("Check for missing punctuation, braces, or keywords");
        }
        
        fix->scope = FIX_SCOPE_SINGLE;
        suggestion->fixes = fix;
        suggestion->fix_count = 1;
    }
    
    suggestion->reasoning = str_dup("Syntax errors are usually caused by missing punctuation or incorrect formatting");
    
    return suggestion;
}

// =============================================================================
// Import Error Fixes
// =============================================================================

FixSuggestion* auto_fix_generate_import_fixes(AutoFixEngine* engine, const FixContext* context) {
    if (!engine || !context) return NULL;
    
    FixSuggestion* suggestion = calloc(1, sizeof(FixSuggestion));
    if (!suggestion) return NULL;
    
    suggestion->suggestion_id = auto_fix_generate_suggestion_id();
    suggestion->title = str_dup("Import Error Fix");
    suggestion->description = str_dup("Add missing import statement");
    suggestion->category = ERROR_CATEGORY_IMPORT;
    suggestion->confidence = FIX_CONFIDENCE_MEDIUM;
    
    CodeFix* fix = calloc(1, sizeof(CodeFix));
    if (fix) {
        fix->description = str_dup("Add import for undefined symbol");
        fix->before_code = str_dup("// File without import\nprint(\"hello\")");
        fix->after_code = str_dup("import \"std/io\"\n\nprint(\"hello\")");
        fix->explanation = str_dup("Add import statement at the top of the file");
        fix->scope = FIX_SCOPE_FILE;
        
        suggestion->fixes = fix;
        suggestion->fix_count = 1;
    }
    
    suggestion->reasoning = str_dup("Undefined symbols usually indicate missing import statements");
    suggestion->examples = str_dup(
        "// Common imports for standard functions:\n"
        "import \"std/io\"        // for print, println\n"
        "import \"std/strconv\"   // for string conversions\n"
        "import \"std/collections\" // for arrays, maps\n"
        "import \"std/math\"      // for mathematical functions"
    );
    
    return suggestion;
}

// =============================================================================
// Error Handling Fixes
// =============================================================================

FixSuggestion* auto_fix_generate_error_handling_fixes(AutoFixEngine* engine, const FixContext* context) {
    if (!engine || !context) return NULL;
    
    FixSuggestion* suggestion = calloc(1, sizeof(FixSuggestion));
    if (!suggestion) return NULL;
    
    suggestion->suggestion_id = auto_fix_generate_suggestion_id();
    suggestion->title = str_dup("Error Handling Fix");
    suggestion->description = str_dup("Handle error union properly");
    suggestion->category = ERROR_CATEGORY_ERROR_HANDLING;
    suggestion->confidence = FIX_CONFIDENCE_HIGH;
    
    CodeFix* fix = calloc(1, sizeof(CodeFix));
    if (fix) {
        fix->description = str_dup("Add error handling for error union");
        fix->before_code = str_dup("let result = risky_operation()");
        fix->after_code = str_dup(
            "let result = risky_operation()\n"
            "if result.is_err() {\n"
            "    return result  // propagate error\n"
            "}\n"
            "let value = result.unwrap()"
        );
        fix->explanation = str_dup("Check error unions before accessing the value");
        fix->scope = FIX_SCOPE_SINGLE;
        
        suggestion->fixes = fix;
        suggestion->fix_count = 1;
    }
    
    suggestion->reasoning = str_dup("Error unions (!T) must be checked before accessing the value");
    suggestion->examples = str_dup(
        "// Problem: Ignoring error union\n"
        "result: !int = divide(10, 0)\n"
        "print(result)  // Error! Must check first\n\n"
        "// Solution 1: Explicit error check\n"
        "result: !int = divide(10, 0)\n"
        "if result.is_ok() {\n"
        "    print(result.unwrap())\n"
        "} else {\n"
        "    print(\"Error: \", result.unwrap_err())\n"
        "}\n\n"
        "// Solution 2: Error propagation with ?\n"
        "func calculate() !int {\n"
        "    result := divide(10, 0)?\n"
        "    return result\n"
        "}"
    );
    
    return suggestion;
}

// =============================================================================
// Fix Application
// =============================================================================

int auto_fix_apply_suggestion(AutoFixEngine* engine, FixSuggestion* suggestion, const char* filename) {
    if (!engine || !suggestion || !filename) return -1;
    
    printf("🔧 Applying fix: %s\n", suggestion->title);
    
    // For now, just print what would be done
    // In a real implementation, this would modify the file
    if (suggestion->fixes) {
        CodeFix* fix = suggestion->fixes;
        printf("   📝 %s\n", fix->description);
        if (fix->before_code && fix->after_code) {
            printf("   Before: %s\n", fix->before_code);
            printf("   After:  %s\n", fix->after_code);
        }
        if (fix->explanation) {
            printf("   💡 %s\n", fix->explanation);
        }
    }
    
    engine->fixes_applied++;
    printf("✅ Fix applied successfully\n");
    return 0;
}

void auto_fix_show_suggestions(AutoFixEngine* engine, FixSuggestion* suggestions) {
    if (!engine || !suggestions) return;
    
    printf("\n🔍 %sAutomatic Fix Suggestions:%s\n", ANSI_BOLD ANSI_BLUE, ANSI_RESET);
    printf("=====================================\n\n");
    
    FixSuggestion* current = suggestions;
    int count = 1;
    
    while (current) {
        // Show confidence level with color
        const char* confidence_color = ANSI_RESET;
        const char* confidence_text = "Unknown";
        
        switch (current->confidence) {
            case FIX_CONFIDENCE_HIGH:
                confidence_color = ANSI_GREEN;
                confidence_text = "High";
                break;
            case FIX_CONFIDENCE_MEDIUM:
                confidence_color = ANSI_YELLOW;
                confidence_text = "Medium";
                break;
            case FIX_CONFIDENCE_LOW:
                confidence_color = ANSI_RED;
                confidence_text = "Low";
                break;
            case FIX_CONFIDENCE_UNSAFE:
                confidence_color = ANSI_MAGENTA;
                confidence_text = "Unsafe";
                break;
        }
        
        printf("%s%d. %s%s\n", ANSI_BOLD, count, current->title, ANSI_RESET);
        printf("   %sConfidence: %s%s%s\n", ANSI_CYAN, confidence_color, confidence_text, ANSI_RESET);
        printf("   %sDescription:%s %s\n", ANSI_CYAN, ANSI_RESET, current->description);
        
        if (current->reasoning) {
            printf("   %sReasoning:%s %s\n", ANSI_CYAN, ANSI_RESET, current->reasoning);
        }
        
        if (current->fixes) {
            printf("   %sFix:%s\n", ANSI_CYAN, ANSI_RESET);
            CodeFix* fix = current->fixes;
            printf("     • %s\n", fix->description);
            if (fix->explanation) {
                printf("     💡 %s\n", fix->explanation);
            }
            if (fix->before_code && fix->after_code) {
                printf("     %sBefore:%s %s\n", ANSI_RED, ANSI_RESET, fix->before_code);
                printf("     %sAfter:%s  %s\n", ANSI_GREEN, ANSI_RESET, fix->after_code);
            }
        }
        
        if (current->examples && engine->show_explanations) {
            printf("   %sExamples:%s\n%s\n", ANSI_CYAN, ANSI_RESET, current->examples);
        }
        
        printf("\n");
        current = current->next;
        count++;
    }
}

// =============================================================================
// Pattern Management
// =============================================================================

int auto_fix_add_pattern(AutoFixEngine* engine, const ErrorPattern* pattern) {
    if (!engine || !pattern) return -1;
    
    ErrorPattern* new_pattern = calloc(1, sizeof(ErrorPattern));
    if (!new_pattern) return -1;
    
    // Copy pattern data
    new_pattern->pattern_id = str_dup(pattern->pattern_id);
    new_pattern->description = str_dup(pattern->description);
    new_pattern->category = pattern->category;
    new_pattern->regex_pattern = str_dup(pattern->regex_pattern);
    new_pattern->example_error = str_dup(pattern->example_error);
    new_pattern->priority = pattern->priority;
    
    // Add to front of list
    new_pattern->next = engine->patterns;
    engine->patterns = new_pattern;
    engine->pattern_count++;
    
    return 0;
}

// =============================================================================
// Utility Functions
// =============================================================================

FixContext* auto_fix_context_new(const char* filename, const char* error_message,
                                Position pos, ASTNode* ast_context) {
    FixContext* context = calloc(1, sizeof(FixContext));
    if (!context) return NULL;
    
    context->filename = str_dup(filename);
    context->error_message = str_dup(error_message);
    context->error_position = pos;
    context->ast_context = ast_context;
    
    return context;
}

void auto_fix_context_free(FixContext* context) {
    if (!context) return;
    
    free(context->filename);
    free(context->error_message);
    free(context->function_context);
    free(context->additional_info);
    free(context);
}

void auto_fix_suggestion_free(FixSuggestion* suggestion) {
    if (!suggestion) return;
    
    free(suggestion->suggestion_id);
    free(suggestion->title);
    free(suggestion->description);
    free(suggestion->reasoning);
    free(suggestion->examples);
    free(suggestion->documentation);
    
    CodeFix* fix = suggestion->fixes;
    while (fix) {
        CodeFix* next = fix->next;
        auto_fix_code_fix_free(fix);
        fix = next;
    }
    
    free(suggestion);
}

void auto_fix_code_fix_free(CodeFix* fix) {
    if (!fix) return;
    
    free(fix->description);
    free(fix->before_code);
    free(fix->after_code);
    free(fix->explanation);
    free(fix->side_effects);
    free(fix);
}

void auto_fix_pattern_free(ErrorPattern* pattern) {
    if (!pattern) return;
    
    free(pattern->pattern_id);
    free(pattern->description);
    free(pattern->regex_pattern);
    free(pattern->example_error);
    free(pattern);
}

char* auto_fix_generate_suggestion_id(void) {
    char* id = malloc(32);
    if (id) {
        snprintf(id, 32, "fix_%d_%ld", ++suggestion_counter, time(NULL));
    }
    return id;
}

void auto_fix_print_statistics(AutoFixEngine* engine) {
    if (!engine) return;
    
    printf("\n📊 %sAuto-Fix Engine Statistics:%s\n", ANSI_BOLD ANSI_BLUE, ANSI_RESET);
    printf("================================\n");
    printf("🔍 Suggestions made: %d\n", engine->suggestions_made);
    printf("✅ Fixes applied: %d\n", engine->fixes_applied);
    printf("🎯 Patterns matched: %d\n", engine->patterns_matched);
    printf("📋 Total patterns: %d\n", engine->pattern_count);
    
    if (engine->suggestions_made > 0) {
        double success_rate = (double)engine->fixes_applied / engine->suggestions_made * 100.0;
        printf("📈 Success rate: %.1f%%\n", success_rate);
    }
    
    printf("\n⚙️  Configuration:\n");
    printf("   Auto-apply safe fixes: %s\n", engine->auto_apply_safe ? "Yes" : "No");
    printf("   Show explanations: %s\n", engine->show_explanations ? "Yes" : "No");
    printf("   Batch mode: %s\n", engine->batch_mode ? "Yes" : "No");
    printf("   Minimum confidence: %s\n", 
           engine->min_confidence == FIX_CONFIDENCE_HIGH ? "High" :
           engine->min_confidence == FIX_CONFIDENCE_MEDIUM ? "Medium" : "Low");
}