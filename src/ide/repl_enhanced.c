#include "repl_syntax.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

// Enhanced REPL with syntax highlighting and completion
typedef struct {
    char* buffer;
    int buffer_size;
    int cursor_pos;
    int buffer_len;
    bool syntax_enabled;
    bool completion_enabled;
    struct termios original_termios;
    bool raw_mode;
} EnhancedREPL;

static EnhancedREPL* g_enhanced_repl = NULL;

// Terminal control functions
static void enter_raw_mode(void) {
    if (g_enhanced_repl->raw_mode) return;
    
    tcgetattr(STDIN_FILENO, &g_enhanced_repl->original_termios);
    struct termios raw = g_enhanced_repl->original_termios;
    
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= CS8;
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_enhanced_repl->raw_mode = true;
}

static void exit_raw_mode(void) {
    if (!g_enhanced_repl->raw_mode) return;
    
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_enhanced_repl->original_termios);
    g_enhanced_repl->raw_mode = false;
}

// Initialize enhanced REPL
bool repl_enhanced_init(void) {
    g_enhanced_repl = malloc(sizeof(EnhancedREPL));
    if (!g_enhanced_repl) return false;
    
    g_enhanced_repl->buffer_size = 1024;
    g_enhanced_repl->buffer = malloc(g_enhanced_repl->buffer_size);
    g_enhanced_repl->cursor_pos = 0;
    g_enhanced_repl->buffer_len = 0;
    g_enhanced_repl->syntax_enabled = true;
    g_enhanced_repl->completion_enabled = true;
    g_enhanced_repl->raw_mode = false;
    
    if (!g_enhanced_repl->buffer) {
        free(g_enhanced_repl);
        g_enhanced_repl = NULL;
        return false;
    }
    
    g_enhanced_repl->buffer[0] = '\0';
    
    // Initialize syntax highlighting system
    return repl_syntax_init();
}

// Cleanup enhanced REPL
void repl_enhanced_cleanup(void) {
    if (!g_enhanced_repl) return;
    
    exit_raw_mode();
    free(g_enhanced_repl->buffer);
    free(g_enhanced_repl);
    g_enhanced_repl = NULL;
    
    repl_syntax_cleanup();
}

// Redraw the current line with syntax highlighting
static void redraw_line(void) {
    if (!g_enhanced_repl) return;
    
    // Clear the line
    printf("\r\033[K");
    
    // Print prompt
    printf("goo> ");
    
    if (g_enhanced_repl->syntax_enabled) {
        // Apply syntax highlighting
        char* highlighted = repl_highlight_line(g_enhanced_repl->buffer, repl_get_current_theme());
        if (highlighted) {
            printf("%s", highlighted);
            free(highlighted);
        } else {
            printf("%s", g_enhanced_repl->buffer);
        }
    } else {
        printf("%s", g_enhanced_repl->buffer);
    }
    
    // Position cursor
    int prompt_len = 5; // "goo> "
    printf("\r\033[%dC", prompt_len + g_enhanced_repl->cursor_pos);
    fflush(stdout);
}

// Insert character at cursor position
static void insert_char(char c) {
    if (!g_enhanced_repl) return;
    
    // Ensure buffer has space
    if (g_enhanced_repl->buffer_len >= g_enhanced_repl->buffer_size - 1) {
        g_enhanced_repl->buffer_size *= 2;
        g_enhanced_repl->buffer = realloc(g_enhanced_repl->buffer, g_enhanced_repl->buffer_size);
        if (!g_enhanced_repl->buffer) return;
    }
    
    // Shift characters to the right
    memmove(g_enhanced_repl->buffer + g_enhanced_repl->cursor_pos + 1,
            g_enhanced_repl->buffer + g_enhanced_repl->cursor_pos,
            g_enhanced_repl->buffer_len - g_enhanced_repl->cursor_pos + 1);
    
    // Insert character
    g_enhanced_repl->buffer[g_enhanced_repl->cursor_pos] = c;
    g_enhanced_repl->cursor_pos++;
    g_enhanced_repl->buffer_len++;
    g_enhanced_repl->buffer[g_enhanced_repl->buffer_len] = '\0';
}

// Delete character at cursor position
static void delete_char(void) {
    if (!g_enhanced_repl || g_enhanced_repl->cursor_pos == 0) return;
    
    // Shift characters to the left
    memmove(g_enhanced_repl->buffer + g_enhanced_repl->cursor_pos - 1,
            g_enhanced_repl->buffer + g_enhanced_repl->cursor_pos,
            g_enhanced_repl->buffer_len - g_enhanced_repl->cursor_pos + 1);
    
    g_enhanced_repl->cursor_pos--;
    g_enhanced_repl->buffer_len--;
}

// Handle tab completion
static void handle_completion(void) {
    if (!g_enhanced_repl || !g_enhanced_repl->completion_enabled) return;
    
    CompletionContext* context = repl_analyze_completion_context(
        g_enhanced_repl->buffer, g_enhanced_repl->cursor_pos);
    if (!context) return;
    
    int count;
    CompletionItem* items = repl_syntax_get_completions(context, &count);
    
    if (items && count > 0) {
        if (count == 1) {
            // Single completion - insert directly
            const char* completion = items[0].insert_text;
            int word_len = strlen(context->current_word);
            
            // Remove current word
            for (int i = 0; i < word_len; i++) {
                delete_char();
            }
            
            // Insert completion
            for (const char* p = completion; *p; p++) {
                insert_char(*p);
            }
        } else {
            // Multiple completions - show menu
            printf("\n");
            repl_show_completion_menu(items, count);
            printf("goo> ");
        }
        
        repl_free_completions(items, count);
    }
    
    repl_free_completion_context(context);
}

// Handle special keys
static bool handle_special_key(int c) {
    if (!g_enhanced_repl) return false;
    
    switch (c) {
        case '\t': // Tab - completion
            handle_completion();
            return true;
            
        case 127: // Backspace
        case '\b':
            delete_char();
            return true;
            
        case '\033': { // Escape sequence
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) return true;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return true;
            
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A': // Up arrow
                        // TODO: History navigation
                        return true;
                    case 'B': // Down arrow
                        // TODO: History navigation
                        return true;
                    case 'C': // Right arrow
                        if (g_enhanced_repl->cursor_pos < g_enhanced_repl->buffer_len) {
                            g_enhanced_repl->cursor_pos++;
                        }
                        return true;
                    case 'D': // Left arrow
                        if (g_enhanced_repl->cursor_pos > 0) {
                            g_enhanced_repl->cursor_pos--;
                        }
                        return true;
                }
            }
            return true;
        }
        
        case 4: // Ctrl+D
            if (g_enhanced_repl->buffer_len == 0) {
                printf("\nExiting...\n");
                exit(0);
            }
            return true;
            
        case 12: // Ctrl+L
            printf("\033[2J\033[H"); // Clear screen
            return true;
            
        default:
            return false;
    }
}

// Main enhanced REPL loop
void repl_enhanced_run(void) {
    if (!repl_enhanced_init()) {
        fprintf(stderr, "Failed to initialize enhanced REPL\n");
        return;
    }
    
    printf("🚀 Goo Enhanced REPL with Syntax Highlighting\n");
    printf("Features: Syntax highlighting, code completion, parentheses matching\n");
    printf("Press Tab for completion, Ctrl+L to clear, Ctrl+D to exit\n\n");
    
    enter_raw_mode();
    
    while (true) {
        redraw_line();
        
        int c = getchar();
        
        if (c == '\r' || c == '\n') {
            // Execute command
            printf("\n");
            
            if (g_enhanced_repl->buffer_len > 0) {
                // Add to history and execute
                printf("Executing: %s\n", g_enhanced_repl->buffer);
                
                // Here we would integrate with the main REPL execution engine
                // For now, just echo back
                if (strcmp(g_enhanced_repl->buffer, "exit") == 0 ||
                    strcmp(g_enhanced_repl->buffer, "quit") == 0) {
                    break;
                }
                
                if (strcmp(g_enhanced_repl->buffer, "syntax off") == 0) {
                    g_enhanced_repl->syntax_enabled = false;
                    printf("Syntax highlighting disabled\n");
                } else if (strcmp(g_enhanced_repl->buffer, "syntax on") == 0) {
                    g_enhanced_repl->syntax_enabled = true;
                    printf("Syntax highlighting enabled\n");
                } else if (strcmp(g_enhanced_repl->buffer, "completion off") == 0) {
                    g_enhanced_repl->completion_enabled = false;
                    printf("Code completion disabled\n");
                } else if (strcmp(g_enhanced_repl->buffer, "completion on") == 0) {
                    g_enhanced_repl->completion_enabled = true;
                    printf("Code completion enabled\n");
                } else if (strcmp(g_enhanced_repl->buffer, "help") == 0) {
                    printf("Enhanced REPL Commands:\n");
                    printf("  syntax on/off     - Toggle syntax highlighting\n");
                    printf("  completion on/off - Toggle code completion\n");
                    printf("  help              - Show this help\n");
                    printf("  exit/quit         - Exit REPL\n");
                    printf("\nKey bindings:\n");
                    printf("  Tab               - Trigger completion\n");
                    printf("  Ctrl+L            - Clear screen\n");
                    printf("  Ctrl+D            - Exit (on empty line)\n");
                    printf("  Arrow keys        - Navigate cursor\n");
                } else {
                    // Simulate evaluation result
                    printf("Result: [evaluation would happen here]\n");
                }
            }
            
            // Clear buffer for next input
            g_enhanced_repl->buffer[0] = '\0';
            g_enhanced_repl->buffer_len = 0;
            g_enhanced_repl->cursor_pos = 0;
            
        } else if (!handle_special_key(c)) {
            // Regular character
            if (c >= 32 && c < 127) { // Printable ASCII
                insert_char(c);
            }
        }
    }
    
    exit_raw_mode();
    repl_enhanced_cleanup();
    printf("Goodbye!\n");
}

// Demo function to showcase syntax highlighting
void repl_syntax_demo(void) {
    if (!repl_syntax_init()) {
        printf("Failed to initialize syntax system\n");
        return;
    }
    
    printf("🎨 Goo Syntax Highlighting Demo\n\n");
    
    const char* demo_lines[] = {
        "// This is a comment",
        "func main() {",
        "    var x int = 42",
        "    var y string = \"Hello, Goo!\"",
        "    var result !int = divide(x, 2)",
        "    var optional ?string = getString()",
        "    ",
        "    if result! > 0 {",
        "        println(\"Success:\", result!)",
        "    }",
        "    ",
        "    ch := make(chan int, 10)",
        "    go processData(ch)",
        "    ",
        "    for i := 0; i < 10; i++ {",
        "        ch <- i * 2",
        "    }",
        "}"
    };
    
    int num_lines = sizeof(demo_lines) / sizeof(demo_lines[0]);
    
    for (int i = 0; i < num_lines; i++) {
        char* highlighted = repl_highlight_line(demo_lines[i], repl_get_current_theme());
        if (highlighted) {
            printf("%3d: %s\n", i + 1, highlighted);
            free(highlighted);
        } else {
            printf("%3d: %s\n", i + 1, demo_lines[i]);
        }
    }
    
    printf("\n🔍 Completion Demo\n");
    
    const char* completion_tests[] = {
        "i",      // Should suggest "if", "int", etc.
        "str",    // Should suggest "string"
        "func",   // Should suggest function template
        "for",    // Should suggest for loop template
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
    
    repl_syntax_cleanup();
}

// Test function for the enhanced REPL
int main(void) {
    printf("Enhanced REPL Test\n");
    printf("1. Run syntax demo\n");
    printf("2. Run enhanced REPL\n");
    printf("Choice (1 or 2): ");
    
    int choice;
    if (scanf("%d", &choice) == 1) {
        getchar(); // consume newline
        
        if (choice == 1) {
            repl_syntax_demo();
        } else if (choice == 2) {
            repl_enhanced_run();
        } else {
            printf("Invalid choice\n");
        }
    }
    
    return 0;
}