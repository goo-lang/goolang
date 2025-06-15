#include "repl_syntax.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

// Simplified enhanced REPL without LSP dependencies
typedef struct {
    char* buffer;
    int buffer_size;
    int cursor_pos;
    int buffer_len;
    bool syntax_enabled;
    bool completion_enabled;
    struct termios original_termios;
    bool raw_mode;
} SimpleEnhancedREPL;

static SimpleEnhancedREPL* g_simple_repl = NULL;

// Initialize simple enhanced REPL
bool simple_repl_init(void) {
    g_simple_repl = malloc(sizeof(SimpleEnhancedREPL));
    if (!g_simple_repl) return false;
    
    g_simple_repl->buffer_size = 1024;
    g_simple_repl->buffer = malloc(g_simple_repl->buffer_size);
    g_simple_repl->cursor_pos = 0;
    g_simple_repl->buffer_len = 0;
    g_simple_repl->syntax_enabled = true;
    g_simple_repl->completion_enabled = true;
    g_simple_repl->raw_mode = false;
    
    if (!g_simple_repl->buffer) {
        free(g_simple_repl);
        g_simple_repl = NULL;
        return false;
    }
    
    g_simple_repl->buffer[0] = '\0';
    
    // Initialize syntax highlighting system
    return repl_syntax_init();
}

// Cleanup simple enhanced REPL
void simple_repl_cleanup(void) {
    if (!g_simple_repl) return;
    
    free(g_simple_repl->buffer);
    free(g_simple_repl);
    g_simple_repl = NULL;
    
    repl_syntax_cleanup();
}

// Demo function to showcase syntax highlighting
void simple_repl_syntax_demo(void) {
    if (!repl_syntax_init()) {
        printf("Failed to initialize syntax system\n");
        return;
    }
    
    printf("🎨 Goo Syntax Highlighting Demo\n\n");
    
    const char* demo_lines[] = {
        "// This is a comment showing Goo language features",
        "package main",
        "",
        "import (",
        "    \"fmt\"",
        "    \"time\"",
        ")",
        "",
        "// Function with error unions and nullable types",
        "func processData(input string) (!Result, !Error) {",
        "    if input == \"\" {",
        "        return nil, Error(\"empty input\")",
        "    }",
        "    ",
        "    var result !Result",
        "    var user ?User = getUser()",
        "    ",
        "    // Error union usage",
        "    if user? {",
        "        result = calculate(user!.id, input)",
        "        if result! {",
        "            fmt.Printf(\"Success: %v\\n\", result!)",
        "        }",
        "    }",
        "    ",
        "    // Channel operations",
        "    ch := make(chan int, 10)",
        "    go func() {",
        "        for i := 0; i < 10; i++ {",
        "            ch <- i * 2",
        "        }",
        "        close(ch)",
        "    }()",
        "    ",
        "    // Receive from channel",
        "    for value := range ch {",
        "        fmt.Printf(\"Received: %d\\n\", value)",
        "    }",
        "    ",
        "    return result, nil",
        "}",
        "",
        "func main() {",
        "    // Various data types",
        "    var x int = 42",
        "    var y float64 = 3.14159",
        "    var message string = \"Hello, Goo!\"",
        "    var isValid bool = true",
        "    ",
        "    // Process data with error handling",
        "    result, err := processData(message)",
        "    if err! {",
        "        fmt.Printf(\"Error: %s\\n\", err!)",
        "        return",
        "    }",
        "    ",
        "    fmt.Printf(\"Final result: %v\\n\", result)",
        "}"
    };
    
    int num_lines = sizeof(demo_lines) / sizeof(demo_lines[0]);
    
    printf("Highlighted Goo code:\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    
    for (int i = 0; i < num_lines; i++) {
        char* highlighted = repl_highlight_line(demo_lines[i], repl_get_current_theme());
        if (highlighted) {
            printf("%3d: %s\n", i + 1, highlighted);
            free(highlighted);
        } else {
            printf("%3d: %s\n", i + 1, demo_lines[i]);
        }
    }
    
    printf("═══════════════════════════════════════════════════════════════\n");
    
    printf("\n🔍 Code Completion Demo\n");
    
    const char* completion_tests[] = {
        "i",      // Should suggest "if", "int", etc.
        "str",    // Should suggest "string"
        "func",   // Should suggest function template
        "for",    // Should suggest for loop template
        "ch",     // Should suggest "chan"
        "erro",   // Should suggest error-related completions
    };
    
    int num_tests = sizeof(completion_tests) / sizeof(completion_tests[0]);
    
    for (int i = 0; i < num_tests; i++) {
        printf("\nCompletions for '%s':\n", completion_tests[i]);
        
        CompletionContext* context = repl_analyze_completion_context(completion_tests[i], strlen(completion_tests[i]));
        if (context) {
            int count;
            CompletionItem* items = repl_syntax_get_completions(context, &count);
            if (items && count > 0) {
                repl_show_completion_menu(items, count);
                repl_free_completions(items, count);
            } else {
                printf("  No completions found\n");
            }
            repl_free_completion_context(context);
        }
    }
    
    printf("\n🎯 Theme Demonstration\n");
    
    printf("\nDefault Theme:\n");
    repl_set_theme(repl_get_default_theme());
    char* sample_line = "func calculate(x !int, y ?string) bool { return x! > 0 }";
    char* highlighted_default = repl_highlight_line(sample_line, repl_get_current_theme());
    printf("  %s\n", highlighted_default);
    free(highlighted_default);
    
    printf("\nDark Theme:\n");
    repl_set_theme(repl_get_dark_theme());
    char* highlighted_dark = repl_highlight_line(sample_line, repl_get_current_theme());
    printf("  %s\n", highlighted_dark);
    free(highlighted_dark);
    
    printf("\nLight Theme:\n");
    repl_set_theme(repl_get_light_theme());
    char* highlighted_light = repl_highlight_line(sample_line, repl_get_current_theme());
    printf("  %s\n", highlighted_light);
    free(highlighted_light);
    
    // Reset to default theme
    repl_set_theme(repl_get_default_theme());
    
    repl_syntax_cleanup();
}

// Simple interactive mode
void simple_repl_interactive(void) {
    if (!simple_repl_init()) {
        printf("Failed to initialize simple enhanced REPL\n");
        return;
    }
    
    printf("🚀 Goo Simple Enhanced REPL\n");
    printf("Features: Basic syntax highlighting and code completion\n");
    printf("Type 'help' for commands, 'exit' to quit\n\n");
    
    char input[1024];
    
    while (true) {
        printf("goo> ");
        fflush(stdout);
        
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        // Remove newline
        input[strcspn(input, "\n")] = 0;
        
        if (strlen(input) == 0) {
            continue;
        }
        
        if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
            break;
        } else if (strcmp(input, "help") == 0) {
            printf("Simple Enhanced REPL Commands:\n");
            printf("  help       - Show this help\n");
            printf("  syntax     - Toggle syntax highlighting\n");
            printf("  completion - Toggle code completion\n");
            printf("  demo       - Run syntax demo\n");
            printf("  exit/quit  - Exit REPL\n");
            printf("\nFeatures:\n");
            printf("  - Real-time syntax highlighting\n");
            printf("  - Code completion suggestions\n");
            printf("  - Goo-specific syntax support\n");
            printf("  - Multiple color themes\n");
        } else if (strcmp(input, "syntax") == 0) {
            g_simple_repl->syntax_enabled = !g_simple_repl->syntax_enabled;
            printf("Syntax highlighting %s\n", 
                   g_simple_repl->syntax_enabled ? "enabled" : "disabled");
        } else if (strcmp(input, "completion") == 0) {
            g_simple_repl->completion_enabled = !g_simple_repl->completion_enabled;
            printf("Code completion %s\n", 
                   g_simple_repl->completion_enabled ? "enabled" : "disabled");
        } else if (strcmp(input, "demo") == 0) {
            simple_repl_syntax_demo();
        } else {
            // Show highlighted version of the input
            if (g_simple_repl->syntax_enabled) {
                char* highlighted = repl_highlight_line(input, repl_get_current_theme());
                if (highlighted) {
                    printf("Highlighted: %s\n", highlighted);
                    free(highlighted);
                }
            }
            
            // Show completions if enabled
            if (g_simple_repl->completion_enabled) {
                CompletionContext* context = repl_analyze_completion_context(input, strlen(input));
                if (context) {
                    int count;
                    CompletionItem* items = repl_syntax_get_completions(context, &count);
                    if (items && count > 0) {
                        printf("Available completions:\n");
                        repl_show_completion_menu(items, count);
                        repl_free_completions(items, count);
                    }
                    repl_free_completion_context(context);
                }
            }
            
            printf("Evaluating: %s\n", input);
        }
    }
    
    simple_repl_cleanup();
    printf("Goodbye!\n");
}

// Main function
int main(void) {
    printf("🎨 Goo Enhanced REPL with Syntax Highlighting\n");
    printf("═══════════════════════════════════════════════\n");
    printf("1. Run syntax highlighting demo\n");
    printf("2. Run interactive REPL\n");
    printf("3. Run both demo and interactive mode\n");
    printf("\nChoice (1-3): ");
    
    char choice_input[10];
    if (fgets(choice_input, sizeof(choice_input), stdin)) {
        int choice = atoi(choice_input);
        
        switch (choice) {
            case 1:
                simple_repl_syntax_demo();
                break;
            case 2:
                simple_repl_interactive();
                break;
            case 3:
                simple_repl_syntax_demo();
                printf("\n" "Press Enter to continue to interactive mode...");
                getchar();
                simple_repl_interactive();
                break;
            default:
                printf("Invalid choice. Running syntax demo...\n");
                simple_repl_syntax_demo();
                break;
        }
    } else {
        simple_repl_syntax_demo();
    }
    
    return 0;
}