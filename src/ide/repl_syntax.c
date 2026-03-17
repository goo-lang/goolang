#include "repl_syntax.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

// Global state
static SyntaxTheme* g_current_theme = NULL;
static SyntaxConfig* g_syntax_config = NULL;
static TerminalCapabilities* g_terminal_caps = NULL;

// Goo language keywords
static const char* GOO_KEYWORDS[] = {
    "if", "else", "for", "while", "break", "continue", "return",
    "fn", "struct", "enum", "interface", "type", "var", "const",
    "import", "package", "go", "defer", "select", "case", "default",
    "switch", "range", "map", "chan", "make", "new", "len", "cap",
    "append", "copy", "delete", "panic", "recover", "close",
    "true", "false", "nil", "iota"
};
static const int GOO_KEYWORDS_COUNT = sizeof(GOO_KEYWORDS) / sizeof(GOO_KEYWORDS[0]);

// Goo language types
static const char* GOO_TYPES[] = {
    "int", "int8", "int16", "int32", "int64",
    "uint", "uint8", "uint16", "uint32", "uint64",
    "float32", "float64", "complex64", "complex128",
    "bool", "string", "byte", "rune", "uintptr",
    "error", "interface{}", "any"
};
static const int GOO_TYPES_COUNT = sizeof(GOO_TYPES) / sizeof(GOO_TYPES[0]);

// Goo language operators
static const char* GOO_OPERATORS[] = {
    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "&^",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "&^=",
    "&&", "||", "<-", "++", "--", "==", "<", ">", "=", "!", "!=", "<=", ">=",
    ":=", "...", "(", ")", "[", "]", "{", "}", ",", ".", ";", ":", "?", "!"
};
static const int GOO_OPERATORS_COUNT = sizeof(GOO_OPERATORS) / sizeof(GOO_OPERATORS[0]);

// Initialize syntax highlighting system
bool repl_syntax_init(void) {
    // Initialize configuration
    g_syntax_config = malloc(sizeof(SyntaxConfig));
    if (!g_syntax_config) return false;
    
    g_syntax_config->enable_syntax_highlighting = true;
    g_syntax_config->enable_completion = true;
    g_syntax_config->enable_realtime_highlighting = true;
    g_syntax_config->enable_paren_matching = true;
    g_syntax_config->enable_error_highlighting = true;
    g_syntax_config->case_sensitive_completion = false;
    g_syntax_config->completion_menu_max_items = 20;
    g_syntax_config->completion_trigger_length = 2;
    g_syntax_config->theme_name = strdup("default");
    
    // Detect terminal capabilities
    g_terminal_caps = repl_detect_terminal_capabilities();
    
    // Set default theme
    g_current_theme = repl_get_default_theme();
    
    return true;
}

// Cleanup syntax highlighting system
void repl_syntax_cleanup(void) {
    if (g_syntax_config) {
        free(g_syntax_config->theme_name);
        free(g_syntax_config);
        g_syntax_config = NULL;
    }
    
    if (g_current_theme) {
        free(g_current_theme);
        g_current_theme = NULL;
    }
    
    if (g_terminal_caps) {
        free(g_terminal_caps);
        g_terminal_caps = NULL;
    }
}

// Detect terminal capabilities
TerminalCapabilities* repl_detect_terminal_capabilities(void) {
    TerminalCapabilities* caps = malloc(sizeof(TerminalCapabilities));
    if (!caps) return NULL;
    
    // Check if stdout is a terminal
    caps->supports_color = isatty(STDOUT_FILENO);
    caps->supports_cursor_movement = caps->supports_color;
    caps->supports_clear_line = caps->supports_color;
    
    // Get terminal size
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        caps->terminal_width = w.ws_col;
        caps->terminal_height = w.ws_row;
    } else {
        caps->terminal_width = 80;
        caps->terminal_height = 24;
    }
    
    return caps;
}

// Get default theme
SyntaxTheme* repl_get_default_theme(void) {
    SyntaxTheme* theme = malloc(sizeof(SyntaxTheme));
    if (!theme) return NULL;
    
    theme->keyword_color = ANSI_BLUE ANSI_BOLD;
    theme->type_color = ANSI_GREEN ANSI_BOLD;
    theme->string_color = ANSI_YELLOW;
    theme->number_color = ANSI_MAGENTA;
    theme->comment_color = ANSI_BRIGHT_BLACK;
    theme->operator_color = ANSI_RED;
    theme->identifier_color = ANSI_RESET;
    theme->constant_color = ANSI_CYAN ANSI_BOLD;
    theme->function_color = ANSI_BRIGHT_BLUE;
    theme->error_color = ANSI_RED ANSI_BG_WHITE;
    theme->match_paren_color = ANSI_GREEN ANSI_BG_BLACK;
    theme->error_paren_color = ANSI_RED ANSI_BG_YELLOW;
    theme->goo_specific_color = ANSI_BRIGHT_MAGENTA;
    
    return theme;
}

// Get dark theme
SyntaxTheme* repl_get_dark_theme(void) {
    SyntaxTheme* theme = malloc(sizeof(SyntaxTheme));
    if (!theme) return NULL;
    
    theme->keyword_color = ANSI_BRIGHT_BLUE;
    theme->type_color = ANSI_BRIGHT_GREEN;
    theme->string_color = ANSI_BRIGHT_YELLOW;
    theme->number_color = ANSI_BRIGHT_MAGENTA;
    theme->comment_color = ANSI_BRIGHT_BLACK;
    theme->operator_color = ANSI_BRIGHT_RED;
    theme->identifier_color = ANSI_BRIGHT_WHITE;
    theme->constant_color = ANSI_BRIGHT_CYAN;
    theme->function_color = ANSI_CYAN;
    theme->error_color = ANSI_RED ANSI_BG_WHITE;
    theme->match_paren_color = ANSI_GREEN ANSI_BG_BLACK;
    theme->error_paren_color = ANSI_RED ANSI_BG_YELLOW;
    theme->goo_specific_color = ANSI_MAGENTA ANSI_BOLD;
    
    return theme;
}

// Get light theme
SyntaxTheme* repl_get_light_theme(void) {
    SyntaxTheme* theme = malloc(sizeof(SyntaxTheme));
    if (!theme) return NULL;
    
    theme->keyword_color = ANSI_BLUE;
    theme->type_color = ANSI_GREEN;
    theme->string_color = ANSI_RED;
    theme->number_color = ANSI_MAGENTA;
    theme->comment_color = ANSI_BLACK;
    theme->operator_color = ANSI_BLACK ANSI_BOLD;
    theme->identifier_color = ANSI_BLACK;
    theme->constant_color = ANSI_CYAN;
    theme->function_color = ANSI_BLUE ANSI_BOLD;
    theme->error_color = ANSI_WHITE ANSI_BG_RED;
    theme->match_paren_color = ANSI_WHITE ANSI_BG_GREEN;
    theme->error_paren_color = ANSI_WHITE ANSI_BG_RED;
    theme->goo_specific_color = ANSI_MAGENTA;
    
    return theme;
}

// Set current theme
void repl_set_theme(SyntaxTheme* theme) {
    if (g_current_theme) {
        free(g_current_theme);
    }
    g_current_theme = theme;
}

// Get current theme
SyntaxTheme* repl_get_current_theme(void) {
    return g_current_theme;
}

// Check if word is a Goo keyword
bool repl_is_goo_keyword(const char* word) {
    for (int i = 0; i < GOO_KEYWORDS_COUNT; i++) {
        if (strcmp(word, GOO_KEYWORDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Check if word is a Goo type
bool repl_is_goo_type(const char* word) {
    for (int i = 0; i < GOO_TYPES_COUNT; i++) {
        if (strcmp(word, GOO_TYPES[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Check if string is a Goo operator
bool repl_is_goo_operator(const char* op) {
    for (int i = 0; i < GOO_OPERATORS_COUNT; i++) {
        if (strcmp(op, GOO_OPERATORS[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Classify token for syntax highlighting
SyntaxElementType repl_classify_token(const char* token, const SyntaxContext* context) {
    if (!token || !*token) return SYNTAX_NORMAL;
    
    // Skip if we're in a string or comment context
    if (context && (context->in_string || context->in_comment)) {
        if (context->in_string) return SYNTAX_STRING;
        if (context->in_comment) return SYNTAX_COMMENT;
    }
    
    // Check for keywords
    if (repl_is_goo_keyword(token)) {
        return SYNTAX_KEYWORD;
    }
    
    // Check for types
    if (repl_is_goo_type(token)) {
        return SYNTAX_TYPE;
    }
    
    // Check for operators
    if (repl_is_goo_operator(token)) {
        return SYNTAX_OPERATOR;
    }
    
    // Check for numbers
    if (isdigit(token[0]) || (token[0] == '.' && isdigit(token[1]))) {
        return SYNTAX_NUMBER;
    }
    
    // Check for string literals
    if (token[0] == '"' || token[0] == '\'' || token[0] == '`') {
        return SYNTAX_STRING;
    }
    
    // Check for comments
    if (strncmp(token, "//", 2) == 0 || strncmp(token, "/*", 2) == 0) {
        return SYNTAX_COMMENT;
    }
    
    // Check for constants (all uppercase)
    bool all_upper = true;
    for (const char* p = token; *p; p++) {
        if (isalpha(*p) && !isupper(*p)) {
            all_upper = false;
            break;
        }
    }
    if (all_upper && isalpha(token[0])) {
        return SYNTAX_CONSTANT;
    }
    
    // Check for Goo-specific syntax (! and ? operators)
    if (strchr(token, '!') || strchr(token, '?')) {
        return SYNTAX_GOO_SPECIFIC;
    }
    
    // Check for function calls (identifier followed by parentheses)
    if (isalpha(token[0]) || token[0] == '_') {
        return SYNTAX_IDENTIFIER;
    }
    
    return SYNTAX_NORMAL;
}

// Highlight a single line
char* repl_highlight_line(const char* line, const SyntaxTheme* theme) {
    if (!line || !theme || !g_syntax_config->enable_syntax_highlighting || 
        !g_terminal_caps->supports_color) {
        return strdup(line);
    }
    
    size_t len = strlen(line);
    size_t output_size = len * 4; // Conservative estimate with ANSI codes
    char* output = malloc(output_size);
    if (!output) return strdup(line);

    size_t off = 0;
    output[0] = '\0';

    SyntaxContext context = {0};

    #define OUT_APPEND_STR(s) do { \
        if (off < output_size - 1) off += snprintf(output + off, output_size - off, "%s", (s)); \
    } while(0)
    #define OUT_APPEND_CHAR(c) do { \
        if (off < output_size - 1) { output[off++] = (c); output[off] = '\0'; } \
    } while(0)

    const char* p = line;
    while (*p) {
        // Handle whitespace
        if (isspace(*p)) {
            OUT_APPEND_CHAR(*p);
            p++;
            continue;
        }

        // Handle string literals
        if (*p == '"' || *p == '\'' || *p == '`') {
            char delimiter = *p;

            OUT_APPEND_STR(theme->string_color);
            while (*p && (*p != delimiter || context.escape_next)) {
                if (context.escape_next) {
                    context.escape_next = false;
                } else if (*p == '\\') {
                    context.escape_next = true;
                }
                OUT_APPEND_CHAR(*p);
                p++;
            }
            if (*p == delimiter) {
                OUT_APPEND_CHAR(*p);
                p++;
            }
            OUT_APPEND_STR(ANSI_RESET);
            continue;
        }

        // Handle comments
        if (*p == '/' && *(p + 1) == '/') {
            OUT_APPEND_STR(theme->comment_color);
            while (*p) {
                OUT_APPEND_CHAR(*p);
                p++;
            }
            OUT_APPEND_STR(ANSI_RESET);
            break;
        }

        // Handle multi-character operators
        if (*p == '<' && *(p + 1) == '-') {
            OUT_APPEND_STR(theme->goo_specific_color);
            OUT_APPEND_CHAR(*p); p++;
            OUT_APPEND_CHAR(*p); p++;
            OUT_APPEND_STR(ANSI_RESET);
            continue;
        }

        // Handle single-character operators and Goo-specific syntax
        if (strchr("+-*/%&|^<>=!:;,()[]{}?", *p)) {
            if (*p == '!' || *p == '?') {
                OUT_APPEND_STR(theme->goo_specific_color);
            } else {
                OUT_APPEND_STR(theme->operator_color);
            }
            OUT_APPEND_CHAR(*p);
            OUT_APPEND_STR(ANSI_RESET);
            p++;
            continue;
        }

        // Handle numbers
        if (isdigit(*p) || (*p == '.' && isdigit(*(p + 1)))) {
            OUT_APPEND_STR(theme->number_color);
            while (*p && (isdigit(*p) || *p == '.' || *p == 'e' || *p == 'E' ||
                         *p == '+' || *p == '-' || *p == 'x' || *p == 'X' ||
                         (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
                OUT_APPEND_CHAR(*p);
                p++;
            }
            OUT_APPEND_STR(ANSI_RESET);
            continue;
        }

        // Handle identifiers, keywords, and types
        if (isalpha(*p) || *p == '_') {
            const char* start = p;
            while (*p && (isalnum(*p) || *p == '_')) {
                p++;
            }

            size_t word_len = p - start;
            char* word = malloc(word_len + 1);
            strncpy(word, start, word_len);
            word[word_len] = '\0';

            SyntaxElementType type = repl_classify_token(word, &context);

            const char* color = theme->identifier_color;
            switch (type) {
                case SYNTAX_KEYWORD:
                    color = theme->keyword_color;
                    break;
                case SYNTAX_TYPE:
                    color = theme->type_color;
                    break;
                case SYNTAX_CONSTANT:
                    color = theme->constant_color;
                    break;
                case SYNTAX_FUNCTION:
                    color = theme->function_color;
                    break;
                case SYNTAX_GOO_SPECIFIC:
                    color = theme->goo_specific_color;
                    break;
                default:
                    color = theme->identifier_color;
                    break;
            }

            OUT_APPEND_STR(color);
            OUT_APPEND_STR(word);
            OUT_APPEND_STR(ANSI_RESET);

            free(word);
            continue;
        }

        // Default: just copy the character
        OUT_APPEND_CHAR(*p);
        p++;
    }

    #undef OUT_APPEND_STR
    #undef OUT_APPEND_CHAR

    return output;
}

// Analyze completion context
CompletionContext* repl_analyze_completion_context(const char* line, int cursor_pos) {
    CompletionContext* context = malloc(sizeof(CompletionContext));
    if (!context) return NULL;
    
    context->line = strdup(line);
    context->cursor_pos = cursor_pos;
    
    // Find word boundaries
    context->word_start = cursor_pos;
    while (context->word_start > 0 && repl_is_identifier_char(line[context->word_start - 1])) {
        context->word_start--;
    }
    
    context->word_end = cursor_pos;
    while (context->word_end < (int)strlen(line) && repl_is_identifier_char(line[context->word_end])) {
        context->word_end++;
    }
    
    // Extract current word
    int word_len = context->word_end - context->word_start;
    context->current_word = malloc(word_len + 1);
    strncpy(context->current_word, line + context->word_start, word_len);
    context->current_word[word_len] = '\0';
    
    // Analyze context
    context->in_string = false;
    context->in_comment = false;
    context->after_operator = false;
    context->in_function_call = false;
    
    // Check if we're in a string or comment
    for (int i = 0; i < cursor_pos; i++) {
        if (line[i] == '"' || line[i] == '\'') {
            context->in_string = !context->in_string;
        } else if (line[i] == '/' && i + 1 < cursor_pos && line[i + 1] == '/') {
            context->in_comment = true;
            break;
        }
    }
    
    // Check if we're after an operator
    if (context->word_start > 0) {
        char prev_char = line[context->word_start - 1];
        context->after_operator = strchr("+-*/=<>!&|", prev_char) != NULL;
    }
    
    // Check if we're in a function call
    for (int i = cursor_pos; i < (int)strlen(line); i++) {
        if (line[i] == '(') {
            context->in_function_call = true;
            break;
        } else if (!isspace(line[i])) {
            break;
        }
    }
    
    // Determine context type
    if (context->in_string) {
        context->context_type = strdup("string");
    } else if (context->in_comment) {
        context->context_type = strdup("comment");
    } else if (context->after_operator) {
        context->context_type = strdup("expression");
    } else if (context->in_function_call) {
        context->context_type = strdup("function_call");
    } else {
        context->context_type = strdup("statement");
    }
    
    return context;
}

// Get completions based on context
CompletionItem* repl_syntax_get_completions(const CompletionContext* context, int* count) {
    if (!context || !count) return NULL;
    
    *count = 0;
    
    // Don't complete in strings or comments
    if (context->in_string || context->in_comment) {
        return NULL;
    }
    
    // Allocate completion items
    CompletionItem* items = malloc(sizeof(CompletionItem) * 100); // Max 100 items
    if (!items) return NULL;
    
    int item_count = 0;
    
    // Add keywords
    for (int i = 0; i < GOO_KEYWORDS_COUNT; i++) {
        if (strlen(context->current_word) == 0 || 
            strncmp(GOO_KEYWORDS[i], context->current_word, strlen(context->current_word)) == 0) {
            
            items[item_count].text = strdup(GOO_KEYWORDS[i]);
            items[item_count].description = strdup("Keyword");
            items[item_count].detail = strdup("Goo language keyword");
            items[item_count].type = COMPLETION_KEYWORD;
            items[item_count].priority = 10;
            items[item_count].insert_text = strdup(GOO_KEYWORDS[i]);
            item_count++;
        }
    }
    
    // Add types
    for (int i = 0; i < GOO_TYPES_COUNT; i++) {
        if (strlen(context->current_word) == 0 || 
            strncmp(GOO_TYPES[i], context->current_word, strlen(context->current_word)) == 0) {
            
            items[item_count].text = strdup(GOO_TYPES[i]);
            items[item_count].description = strdup("Type");
            items[item_count].detail = strdup("Built-in type");
            items[item_count].type = COMPLETION_TYPE;
            items[item_count].priority = 8;
            items[item_count].insert_text = strdup(GOO_TYPES[i]);
            item_count++;
        }
    }
    
    // Add Goo-specific features
    const char* goo_features[] = {
        "error_union!", "nullable?", "chan", "go", "defer", "select"
    };
    const int goo_features_count = sizeof(goo_features) / sizeof(goo_features[0]);
    
    for (int i = 0; i < goo_features_count; i++) {
        if (strlen(context->current_word) == 0 || 
            strncmp(goo_features[i], context->current_word, strlen(context->current_word)) == 0) {
            
            items[item_count].text = strdup(goo_features[i]);
            items[item_count].description = strdup("Goo Feature");
            items[item_count].detail = strdup("Goo-specific language feature");
            items[item_count].type = COMPLETION_GOO_FEATURE;
            items[item_count].priority = 12;
            items[item_count].insert_text = strdup(goo_features[i]);
            item_count++;
        }
    }
    
    // Add common snippets
    if (strcmp(context->context_type, "statement") == 0) {
        const char* snippets[][3] = {
            {"if", "if condition {}", "Conditional statement"},
            {"for", "for condition {}", "Loop statement"},
            {"func", "func name() {}", "Function declaration"},
            {"struct", "struct Name {}", "Struct declaration"}
        };
        const int snippets_count = sizeof(snippets) / sizeof(snippets[0]);
        
        for (int i = 0; i < snippets_count; i++) {
            if (strlen(context->current_word) == 0 || 
                strncmp(snippets[i][0], context->current_word, strlen(context->current_word)) == 0) {
                
                items[item_count].text = strdup(snippets[i][0]);
                items[item_count].description = strdup("Snippet");
                items[item_count].detail = strdup(snippets[i][2]);
                items[item_count].type = COMPLETION_SNIPPET;
                items[item_count].priority = 15;
                items[item_count].insert_text = strdup(snippets[i][1]);
                item_count++;
            }
        }
    }
    
    *count = item_count;
    return items;
}

// Free completion items
void repl_free_completions(CompletionItem* items, int count) {
    if (!items) return;
    
    for (int i = 0; i < count; i++) {
        free(items[i].text);
        free(items[i].description);
        free(items[i].detail);
        free(items[i].insert_text);
    }
    free(items);
}

// Free completion context
void repl_free_completion_context(CompletionContext* context) {
    if (!context) return;
    
    free(context->line);
    free(context->current_word);
    free(context->context_type);
    free(context);
}

// Show completion menu
int repl_show_completion_menu(CompletionItem* items, int count) {
    if (!items || count == 0) return -1;
    
    printf("\n📝 Completions:\n");
    for (int i = 0; i < count && i < g_syntax_config->completion_menu_max_items; i++) {
        const char* type_icon = "·";
        switch (items[i].type) {
            case COMPLETION_KEYWORD: type_icon = "🔑"; break;
            case COMPLETION_TYPE: type_icon = "🏷️"; break;
            case COMPLETION_FUNCTION: type_icon = "⚡"; break;
            case COMPLETION_VARIABLE: type_icon = "📦"; break;
            case COMPLETION_CONSTANT: type_icon = "💎"; break;
            case COMPLETION_SNIPPET: type_icon = "📋"; break;
            case COMPLETION_GOO_FEATURE: type_icon = "🚀"; break;
            default: type_icon = "·"; break;
        }
        
        printf("  %d. %s %-20s %s\n", i + 1, type_icon, items[i].text, items[i].description);
    }
    
    return 0;
}

// Utility functions
bool repl_is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool repl_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool repl_is_alnum(char c) {
    return repl_is_alpha(c) || (c >= '0' && c <= '9');
}

bool repl_is_identifier_char(char c) {
    return repl_is_alnum(c) || c == '_';
}

// Get configuration
SyntaxConfig* repl_get_syntax_config(void) {
    return g_syntax_config;
}

// Terminal control functions
void repl_clear_line(void) {
    if (g_terminal_caps && g_terminal_caps->supports_clear_line) {
        printf("\033[2K\r");
        fflush(stdout);
    }
}

void repl_move_cursor(int col) {
    if (g_terminal_caps && g_terminal_caps->supports_cursor_movement) {
        printf("\033[%dG", col + 1);
        fflush(stdout);
    }
}

// Find matching parenthesis
int repl_find_matching_paren(const char* line, int paren_pos) {
    if (!line || paren_pos < 0 || paren_pos >= (int)strlen(line)) return -1;
    
    char open_paren = line[paren_pos];
    char close_paren;
    int direction;
    
    switch (open_paren) {
        case '(': close_paren = ')'; direction = 1; break;
        case '[': close_paren = ']'; direction = 1; break;
        case '{': close_paren = '}'; direction = 1; break;
        case ')': close_paren = '('; direction = -1; break;
        case ']': close_paren = '['; direction = -1; break;
        case '}': close_paren = '{'; direction = -1; break;
        default: return -1;
    }
    
    int depth = 1;
    int pos = paren_pos + direction;
    
    while (pos >= 0 && pos < (int)strlen(line) && depth > 0) {
        if (line[pos] == open_paren) {
            depth += direction;
        } else if (line[pos] == close_paren) {
            depth -= direction;
        }
        
        if (depth == 0) return pos;
        pos += direction;
    }
    
    return -1;
}

// Highlight matching parentheses
char* repl_highlight_matching_parens(const char* line, int cursor_pos) {
    if (!line || !g_current_theme) return strdup(line);
    
    int match_pos = repl_find_matching_paren(line, cursor_pos);
    if (match_pos == -1) return strdup(line);
    
    size_t len = strlen(line);
    size_t output_size = len * 3; // Space for ANSI codes
    char* output = malloc(output_size);
    if (!output) return strdup(line);

    size_t off = 0;
    output[0] = '\0';

    for (int i = 0; i < (int)len; i++) {
        if (i == cursor_pos || i == match_pos) {
            if (off < output_size - 1)
                off += snprintf(output + off, output_size - off, "%s%c%s",
                                g_current_theme->match_paren_color, line[i], ANSI_RESET);
        } else {
            if (off < output_size - 1) { output[off++] = line[i]; output[off] = '\0'; }
        }
    }

    return output;
}