#ifndef REPL_SYNTAX_H
#define REPL_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>

// ANSI color codes for terminal syntax highlighting
#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_DIM        "\033[2m"
#define ANSI_ITALIC     "\033[3m"
#define ANSI_UNDERLINE  "\033[4m"

// Standard colors
#define ANSI_BLACK      "\033[30m"
#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_WHITE      "\033[37m"

// Bright colors
#define ANSI_BRIGHT_BLACK   "\033[90m"
#define ANSI_BRIGHT_RED     "\033[91m"
#define ANSI_BRIGHT_GREEN   "\033[92m"
#define ANSI_BRIGHT_YELLOW  "\033[93m"
#define ANSI_BRIGHT_BLUE    "\033[94m"
#define ANSI_BRIGHT_MAGENTA "\033[95m"
#define ANSI_BRIGHT_CYAN    "\033[96m"
#define ANSI_BRIGHT_WHITE   "\033[97m"

// Background colors
#define ANSI_BG_BLACK       "\033[40m"
#define ANSI_BG_RED         "\033[41m"
#define ANSI_BG_GREEN       "\033[42m"
#define ANSI_BG_YELLOW      "\033[43m"
#define ANSI_BG_BLUE        "\033[44m"
#define ANSI_BG_MAGENTA     "\033[45m"
#define ANSI_BG_CYAN        "\033[46m"
#define ANSI_BG_WHITE       "\033[47m"

// Syntax element types
typedef enum {
    SYNTAX_NORMAL,
    SYNTAX_KEYWORD,
    SYNTAX_TYPE,
    SYNTAX_STRING,
    SYNTAX_NUMBER,
    SYNTAX_COMMENT,
    SYNTAX_OPERATOR,
    SYNTAX_IDENTIFIER,
    SYNTAX_CONSTANT,
    SYNTAX_FUNCTION,
    SYNTAX_ERROR,
    SYNTAX_MATCH_PAREN,
    SYNTAX_ERROR_PAREN,
    SYNTAX_GOO_SPECIFIC
} SyntaxElementType;

// Syntax highlighting theme
typedef struct {
    const char* keyword_color;     // Keywords (if, for, fn, etc.)
    const char* type_color;        // Types (int, string, bool)
    const char* string_color;      // String literals
    const char* number_color;      // Numeric literals
    const char* comment_color;     // Comments
    const char* operator_color;    // Operators (+, -, !, ?)
    const char* identifier_color;  // Variable names
    const char* constant_color;    // Constants
    const char* function_color;    // Function names
    const char* error_color;       // Error highlighting
    const char* match_paren_color; // Matching parentheses
    const char* error_paren_color; // Unmatched parentheses
    const char* goo_specific_color; // Goo-specific syntax
} SyntaxTheme;

// Completion item types
typedef enum {
    COMPLETION_KEYWORD,
    COMPLETION_TYPE,
    COMPLETION_FUNCTION,
    COMPLETION_VARIABLE,
    COMPLETION_CONSTANT,
    COMPLETION_OPERATOR,
    COMPLETION_SNIPPET,
    COMPLETION_GOO_FEATURE
} CompletionItemType;

// Completion item
typedef struct {
    char* text;
    char* description;
    char* detail;
    CompletionItemType type;
    int priority;
    char* insert_text;  // Text to insert (may differ from display text)
} CompletionItem;

// Completion context
typedef struct {
    char* line;
    int cursor_pos;
    int word_start;
    int word_end;
    char* current_word;
    bool in_string;
    bool in_comment;
    bool after_operator;
    bool in_function_call;
    char* context_type;  // "expression", "statement", "type", etc.
} CompletionContext;

// Syntax highlighting context
typedef struct {
    bool in_string;
    bool in_comment;
    bool in_multiline_comment;
    char string_delimiter;
    int paren_depth;
    int bracket_depth;
    int brace_depth;
    bool escape_next;
} SyntaxContext;

// Terminal capabilities
typedef struct {
    bool supports_color;
    bool supports_cursor_movement;
    bool supports_clear_line;
    int terminal_width;
    int terminal_height;
} TerminalCapabilities;

// Core functions

// Initialize syntax highlighting system
bool repl_syntax_init(void);

// Cleanup syntax highlighting system
void repl_syntax_cleanup(void);

// Detect terminal capabilities
TerminalCapabilities* repl_detect_terminal_capabilities(void);

// Syntax highlighting functions
char* repl_highlight_line(const char* line, const SyntaxTheme* theme);
char* repl_highlight_code(const char* code, const SyntaxTheme* theme);
SyntaxElementType repl_classify_token(const char* token, const SyntaxContext* context);

// Theme management
SyntaxTheme* repl_get_default_theme(void);
SyntaxTheme* repl_get_dark_theme(void);
SyntaxTheme* repl_get_light_theme(void);
void repl_set_theme(SyntaxTheme* theme);
SyntaxTheme* repl_get_current_theme(void);

// Completion system
CompletionItem* repl_syntax_get_completions(const CompletionContext* context, int* count);
CompletionContext* repl_analyze_completion_context(const char* line, int cursor_pos);
void repl_free_completions(CompletionItem* items, int count);
void repl_free_completion_context(CompletionContext* context);

// Interactive completion
int repl_show_completion_menu(CompletionItem* items, int count);
char* repl_complete_word(const char* line, int cursor_pos, int* new_cursor_pos);

// Goo-specific syntax support
bool repl_is_goo_keyword(const char* word);
bool repl_is_goo_type(const char* word);
bool repl_is_goo_operator(const char* op);
CompletionItem* repl_get_goo_specific_completions(const CompletionContext* context, int* count);

// Error highlighting
char* repl_highlight_error(const char* line, int error_start, int error_end, const char* error_message);
void repl_show_error_underline(const char* line, int error_start, int error_end);

// Parentheses matching
int repl_find_matching_paren(const char* line, int paren_pos);
char* repl_highlight_matching_parens(const char* line, int cursor_pos);

// Real-time highlighting during input
void repl_enable_realtime_highlighting(bool enable);
char* repl_process_input_char(char c, const char* current_line, int cursor_pos);

// Line editing helpers
char* repl_insert_completion(const char* line, int cursor_pos, const char* completion, int* new_cursor_pos);
char* repl_format_code(const char* code);
bool repl_is_complete_expression(const char* code);

// Utility functions
bool repl_is_whitespace(char c);
bool repl_is_alpha(char c);
bool repl_is_alnum(char c);
bool repl_is_identifier_char(char c);
int repl_find_word_boundary(const char* line, int pos, bool forward);

// Terminal control functions
void repl_clear_line(void);
void repl_move_cursor(int col);
void repl_save_cursor(void);
void repl_restore_cursor(void);
void repl_set_cursor_style(const char* style);

// Configuration
typedef struct {
    bool enable_syntax_highlighting;
    bool enable_completion;
    bool enable_realtime_highlighting;
    bool enable_paren_matching;
    bool enable_error_highlighting;
    bool case_sensitive_completion;
    int completion_menu_max_items;
    int completion_trigger_length;
    char* theme_name;
} SyntaxConfig;

SyntaxConfig* repl_get_syntax_config(void);
void repl_set_syntax_config(const SyntaxConfig* config);
bool repl_load_syntax_config(const char* config_file);
bool repl_save_syntax_config(const char* config_file);

#endif // REPL_SYNTAX_H