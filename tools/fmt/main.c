#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

// Goo Code Formatter (goo-fmt)
// Formats Goo source code according to configurable style rules

#define MAX_LINE_LENGTH 1024
#define MAX_PATH_LENGTH 1024
#define MAX_TOKEN_LENGTH 256
#define DEFAULT_INDENT_SIZE 4
#define DEFAULT_TAB_WIDTH 4

// Token types for parsing
typedef enum {
    TOKEN_EOF,
    TOKEN_NEWLINE,
    TOKEN_WHITESPACE,
    TOKEN_COMMENT_LINE,
    TOKEN_COMMENT_BLOCK,
    TOKEN_IDENTIFIER,
    TOKEN_KEYWORD,
    TOKEN_STRING,
    TOKEN_CHAR,
    TOKEN_NUMBER,
    TOKEN_OPERATOR,
    TOKEN_PUNCTUATION,
    TOKEN_BRACE_OPEN,
    TOKEN_BRACE_CLOSE,
    TOKEN_PAREN_OPEN,
    TOKEN_PAREN_CLOSE,
    TOKEN_BRACKET_OPEN,
    TOKEN_BRACKET_CLOSE,
    TOKEN_SEMICOLON,
    TOKEN_COMMA,
    TOKEN_DOT
} TokenType;

// Token structure
typedef struct {
    TokenType type;
    char value[MAX_TOKEN_LENGTH];
    int line;
    int column;
    int length;
} Token;

// Formatting configuration
typedef struct {
    int indent_size;
    int tab_width;
    int use_tabs;
    int max_line_length;
    int space_after_comma;
    int space_around_operators;
    int space_before_paren;
    int space_inside_paren;
    int space_before_brace;
    int space_inside_brace;
    int newline_before_brace;
    int align_struct_fields;
    int align_function_params;
    int sort_imports;
    int remove_trailing_whitespace;
    int ensure_newline_at_eof;
    int blank_lines_around_functions;
    int blank_lines_around_types;
    int compact_short_blocks;
} FormatConfig;

// Formatter state
typedef struct {
    char* input;
    size_t input_size;
    size_t input_pos;
    char* output;
    size_t output_size;
    size_t output_capacity;
    int current_line;
    int current_column;
    int indent_level;
    int in_function;
    int in_struct;
    int in_interface;
    int paren_depth;
    int brace_depth;
    int bracket_depth;
    Token current_token;
    Token prev_token;
    FormatConfig config;
} Formatter;

// Keywords for syntax highlighting and formatting
static const char* goo_keywords[] = {
    "package", "import", "func", "type", "struct", "interface", "const", "var",
    "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "go", "defer", "select", "chan", "range", "map",
    "make", "new", "len", "cap", "append", "copy", "delete", "close",
    "true", "false", "nil", "iota", "int", "int8", "int16", "int32", "int64",
    "uint", "uint8", "uint16", "uint32", "uint64", "uintptr", "byte", "rune",
    "float32", "float64", "complex64", "complex128", "bool", "string", "error",
    "unsafe", "comptime", "try", "catch", "match", "pub", "sub", "req", "rep",
    NULL
};

// Function prototypes
void init_config(FormatConfig* config);
void load_config_file(FormatConfig* config, const char* config_path);
void show_help(void);
int format_file(const char* input_path, const char* output_path, FormatConfig* config);
int format_directory(const char* dir_path, FormatConfig* config, int recursive);
int format_string(const char* input, char** output, FormatConfig* config);

// Formatter functions
Formatter* create_formatter(const char* input, FormatConfig* config);
void destroy_formatter(Formatter* fmt);
void next_token(Formatter* fmt);
int is_keyword(const char* word);
void emit_char(Formatter* fmt, char c);
void emit_string(Formatter* fmt, const char* str);
void emit_indent(Formatter* fmt);
void emit_newline(Formatter* fmt);
void emit_space(Formatter* fmt);
void format_tokens(Formatter* fmt);

// Utility functions
int is_goo_file(const char* filename);
void trim_whitespace(char* str);
int ensure_directory(const char* path);

int main(int argc, char* argv[]) {
    FormatConfig config;
    init_config(&config);
    
    const char* config_file = NULL;
    const char* output_file = NULL;
    int write_to_file = 0;
    int recursive = 0;
    int check_only = 0;
    int diff_mode = 0;
    
    if (argc < 2) {
        printf("Goo Code Formatter v0.1.0\n\n");
        printf("Usage: goo-fmt [options] <files...>\n\n");
        show_help();
        return 0;
    }
    
    // Parse command line arguments
    int file_start = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            show_help();
            return 0;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--write") == 0) {
            write_to_file = 1;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--recursive") == 0) {
            recursive = 1;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--check") == 0) {
            check_only = 1;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--diff") == 0) {
            diff_mode = 1;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            }
        } else if (strcmp(argv[i], "--indent") == 0 && i + 1 < argc) {
            config.indent_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tabs") == 0) {
            config.use_tabs = 1;
        } else if (strcmp(argv[i], "--max-line") == 0 && i + 1 < argc) {
            config.max_line_length = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            file_start = i;
            break;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            return 1;
        }
    }
    
    // Load configuration file if specified
    if (config_file) {
        load_config_file(&config, config_file);
    } else {
        // Try to load default config files
        char default_config[MAX_PATH_LENGTH];
        
        // Try .goo-fmt.toml in current directory
        if (access(".goo-fmt.toml", F_OK) == 0) {
            load_config_file(&config, ".goo-fmt.toml");
        }
        // Try ~/.config/goo/fmt.toml
        else {
            const char* home = getenv("HOME");
            if (home) {
                snprintf(default_config, sizeof(default_config), "%s/.config/goo/fmt.toml", home);
                if (access(default_config, F_OK) == 0) {
                    load_config_file(&config, default_config);
                }
            }
        }
    }
    
    if (file_start >= argc) {
        printf("Error: No input files specified\n");
        return 1;
    }
    
    int exit_code = 0;
    int files_formatted = 0;
    int files_changed = 0;
    
    // Process each file/directory argument
    for (int i = file_start; i < argc; i++) {
        const char* path = argv[i];
        struct stat path_stat;
        
        if (stat(path, &path_stat) != 0) {
            printf("Error: Cannot access '%s'\n", path);
            exit_code = 1;
            continue;
        }
        
        if (S_ISDIR(path_stat.st_mode)) {
            if (format_directory(path, &config, recursive) != 0) {
                exit_code = 1;
            }
        } else if (is_goo_file(path)) {
            const char* out_path = output_file ? output_file : (write_to_file ? path : NULL);
            
            if (check_only) {
                // Just check if file needs formatting
                char* original_content = NULL;
                char* formatted_content = NULL;
                
                FILE* file = fopen(path, "r");
                if (file) {
                    fseek(file, 0, SEEK_END);
                    long size = ftell(file);
                    fseek(file, 0, SEEK_SET);
                    
                    original_content = malloc(size + 1);
                    fread(original_content, 1, size, file);
                    original_content[size] = '\0';
                    fclose(file);
                    
                    if (format_string(original_content, &formatted_content, &config) == 0) {
                        if (strcmp(original_content, formatted_content) != 0) {
                            printf("File needs formatting: %s\n", path);
                            files_changed++;
                            exit_code = 1;
                        }
                    }
                    
                    free(original_content);
                    free(formatted_content);
                }
            } else if (diff_mode) {
                // Show diff of changes
                printf("Diff for %s:\n", path);
                // In a real implementation, this would show a proper diff
                printf("(diff functionality not implemented in this demo)\n");
            } else {
                if (format_file(path, out_path, &config) == 0) {
                    files_formatted++;
                    if (!output_file && !write_to_file) {
                        // Output was written to stdout
                    } else {
                        printf("Formatted: %s\n", path);
                    }
                } else {
                    exit_code = 1;
                }
            }
        } else {
            printf("Skipping non-Goo file: %s\n", path);
        }
    }
    
    if (check_only) {
        if (files_changed > 0) {
            printf("\n%d file(s) need formatting\n", files_changed);
        } else {
            printf("All files are properly formatted\n");
        }
    } else if (!check_only && !diff_mode) {
        printf("\nFormatted %d file(s)\n", files_formatted);
    }
    
    return exit_code;
}

void init_config(FormatConfig* config) {
    config->indent_size = DEFAULT_INDENT_SIZE;
    config->tab_width = DEFAULT_TAB_WIDTH;
    config->use_tabs = 0;
    config->max_line_length = 100;
    config->space_after_comma = 1;
    config->space_around_operators = 1;
    config->space_before_paren = 0;
    config->space_inside_paren = 0;
    config->space_before_brace = 1;
    config->space_inside_brace = 0;
    config->newline_before_brace = 0;
    config->align_struct_fields = 1;
    config->align_function_params = 1;
    config->sort_imports = 1;
    config->remove_trailing_whitespace = 1;
    config->ensure_newline_at_eof = 1;
    config->blank_lines_around_functions = 1;
    config->blank_lines_around_types = 1;
    config->compact_short_blocks = 1;
}

void load_config_file(FormatConfig* config, const char* config_path) {
    FILE* file = fopen(config_path, "r");
    if (!file) {
        printf("Warning: Cannot read config file '%s'\n", config_path);
        return;
    }
    
    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), file)) {
        trim_whitespace(line);
        
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        
        // Parse key=value pairs
        char* equals = strchr(line, '=');
        if (equals) {
            *equals = '\0';
            char* key = line;
            char* value = equals + 1;
            
            trim_whitespace(key);
            trim_whitespace(value);
            
            // Parse configuration options
            if (strcmp(key, "indent_size") == 0) {
                config->indent_size = atoi(value);
            } else if (strcmp(key, "tab_width") == 0) {
                config->tab_width = atoi(value);
            } else if (strcmp(key, "use_tabs") == 0) {
                config->use_tabs = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "max_line_length") == 0) {
                config->max_line_length = atoi(value);
            } else if (strcmp(key, "space_after_comma") == 0) {
                config->space_after_comma = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "space_around_operators") == 0) {
                config->space_around_operators = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "space_before_paren") == 0) {
                config->space_before_paren = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "space_inside_paren") == 0) {
                config->space_inside_paren = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "space_before_brace") == 0) {
                config->space_before_brace = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "space_inside_brace") == 0) {
                config->space_inside_brace = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "newline_before_brace") == 0) {
                config->newline_before_brace = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "align_struct_fields") == 0) {
                config->align_struct_fields = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "align_function_params") == 0) {
                config->align_function_params = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "sort_imports") == 0) {
                config->sort_imports = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "remove_trailing_whitespace") == 0) {
                config->remove_trailing_whitespace = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "ensure_newline_at_eof") == 0) {
                config->ensure_newline_at_eof = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "blank_lines_around_functions") == 0) {
                config->blank_lines_around_functions = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "blank_lines_around_types") == 0) {
                config->blank_lines_around_types = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "compact_short_blocks") == 0) {
                config->compact_short_blocks = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            }
        }
    }
    
    fclose(file);
}

void show_help(void) {
    printf("Format Goo source code files according to style guidelines.\n\n");
    printf("Options:\n");
    printf("  -w, --write              Write result to source file instead of stdout\n");
    printf("  -r, --recursive          Process directories recursively\n");
    printf("  -c, --check              Check if files are formatted (exit 1 if not)\n");
    printf("  -d, --diff               Show diff of formatting changes\n");
    printf("  -o, --output <file>      Write output to specified file\n");
    printf("  --config <file>          Use specified configuration file\n");
    printf("  --indent <size>          Set indentation size (default: 4)\n");
    printf("  --tabs                   Use tabs for indentation instead of spaces\n");
    printf("  --max-line <length>      Set maximum line length (default: 100)\n");
    printf("  -h, --help               Show this help message\n");
    
    printf("\nConfiguration:\n");
    printf("The formatter looks for configuration in the following order:\n");
    printf("  1. File specified with --config\n");
    printf("  2. .goo-fmt.toml in current directory\n");
    printf("  3. ~/.config/goo/fmt.toml\n");
    
    printf("\nExamples:\n");
    printf("  goo-fmt main.goo                    # Format and print to stdout\n");
    printf("  goo-fmt -w main.goo                 # Format in place\n");
    printf("  goo-fmt -r src/                     # Format all .goo files in src/\n");
    printf("  goo-fmt -c main.goo                 # Check if file needs formatting\n");
    printf("  goo-fmt --indent 2 -w main.goo     # Format with 2-space indentation\n");
}

int format_file(const char* input_path, const char* output_path, FormatConfig* config) {
    FILE* input_file = fopen(input_path, "r");
    if (!input_file) {
        printf("Error: Cannot open file '%s'\n", input_path);
        return 1;
    }
    
    // Read entire file
    fseek(input_file, 0, SEEK_END);
    long file_size = ftell(input_file);
    fseek(input_file, 0, SEEK_SET);
    
    char* input_content = malloc(file_size + 1);
    fread(input_content, 1, file_size, input_file);
    input_content[file_size] = '\0';
    fclose(input_file);
    
    char* formatted_content = NULL;
    int result = format_string(input_content, &formatted_content, config);
    
    if (result == 0) {
        if (output_path) {
            FILE* output_file = fopen(output_path, "w");
            if (output_file) {
                fprintf(output_file, "%s", formatted_content);
                fclose(output_file);
            } else {
                printf("Error: Cannot write to file '%s'\n", output_path);
                result = 1;
            }
        } else {
            printf("%s", formatted_content);
        }
    }
    
    free(input_content);
    free(formatted_content);
    return result;
}

int format_directory(const char* dir_path, FormatConfig* config, int recursive) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        printf("Error: Cannot open directory '%s'\n", dir_path);
        return 1;
    }
    
    struct dirent* entry;
    int result = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char full_path[MAX_PATH_LENGTH];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        
        struct stat entry_stat;
        if (stat(full_path, &entry_stat) == 0) {
            if (S_ISDIR(entry_stat.st_mode) && recursive) {
                if (format_directory(full_path, config, recursive) != 0) {
                    result = 1;
                }
            } else if (S_ISREG(entry_stat.st_mode) && is_goo_file(entry->d_name)) {
                if (format_file(full_path, full_path, config) != 0) {
                    result = 1;
                }
            }
        }
    }
    
    closedir(dir);
    return result;
}

int format_string(const char* input, char** output, FormatConfig* config) {
    Formatter* fmt = create_formatter(input, config);
    if (!fmt) {
        return 1;
    }
    
    format_tokens(fmt);
    
    // Ensure newline at EOF if configured
    if (config->ensure_newline_at_eof && fmt->output_size > 0 && 
        fmt->output[fmt->output_size - 1] != '\n') {
        emit_newline(fmt);
    }
    
    // Copy output
    *output = malloc(fmt->output_size + 1);
    memcpy(*output, fmt->output, fmt->output_size);
    (*output)[fmt->output_size] = '\0';
    
    destroy_formatter(fmt);
    return 0;
}

Formatter* create_formatter(const char* input, FormatConfig* config) {
    Formatter* fmt = malloc(sizeof(Formatter));
    if (!fmt) {
        return NULL;
    }
    
    fmt->input = strdup(input);
    fmt->input_size = strlen(input);
    fmt->input_pos = 0;
    fmt->output_capacity = fmt->input_size * 2; // Start with double the input size
    fmt->output = malloc(fmt->output_capacity);
    fmt->output_size = 0;
    fmt->current_line = 1;
    fmt->current_column = 1;
    fmt->indent_level = 0;
    fmt->in_function = 0;
    fmt->in_struct = 0;
    fmt->in_interface = 0;
    fmt->paren_depth = 0;
    fmt->brace_depth = 0;
    fmt->bracket_depth = 0;
    fmt->config = *config;
    
    // Initialize tokens
    memset(&fmt->current_token, 0, sizeof(Token));
    memset(&fmt->prev_token, 0, sizeof(Token));
    
    return fmt;
}

void destroy_formatter(Formatter* fmt) {
    if (fmt) {
        free(fmt->input);
        free(fmt->output);
        free(fmt);
    }
}

void next_token(Formatter* fmt) {
    fmt->prev_token = fmt->current_token;
    
    // Skip whitespace
    while (fmt->input_pos < fmt->input_size && isspace(fmt->input[fmt->input_pos])) {
        if (fmt->input[fmt->input_pos] == '\n') {
            fmt->current_line++;
            fmt->current_column = 1;
        } else {
            fmt->current_column++;
        }
        fmt->input_pos++;
    }
    
    if (fmt->input_pos >= fmt->input_size) {
        fmt->current_token.type = TOKEN_EOF;
        return;
    }
    
    char c = fmt->input[fmt->input_pos];
    fmt->current_token.line = fmt->current_line;
    fmt->current_token.column = fmt->current_column;
    
    // Handle different token types
    if (c == '/' && fmt->input_pos + 1 < fmt->input_size) {
        if (fmt->input[fmt->input_pos + 1] == '/') {
            // Line comment
            fmt->current_token.type = TOKEN_COMMENT_LINE;
            int start = fmt->input_pos;
            while (fmt->input_pos < fmt->input_size && fmt->input[fmt->input_pos] != '\n') {
                fmt->input_pos++;
            }
            int len = fmt->input_pos - start;
            strncpy(fmt->current_token.value, &fmt->input[start], len);
            fmt->current_token.value[len] = '\0';
            fmt->current_token.length = len;
            return;
        } else if (fmt->input[fmt->input_pos + 1] == '*') {
            // Block comment
            fmt->current_token.type = TOKEN_COMMENT_BLOCK;
            int start = fmt->input_pos;
            fmt->input_pos += 2;
            while (fmt->input_pos + 1 < fmt->input_size) {
                if (fmt->input[fmt->input_pos] == '*' && fmt->input[fmt->input_pos + 1] == '/') {
                    fmt->input_pos += 2;
                    break;
                }
                if (fmt->input[fmt->input_pos] == '\n') {
                    fmt->current_line++;
                    fmt->current_column = 1;
                } else {
                    fmt->current_column++;
                }
                fmt->input_pos++;
            }
            int len = fmt->input_pos - start;
            strncpy(fmt->current_token.value, &fmt->input[start], len);
            fmt->current_token.value[len] = '\0';
            fmt->current_token.length = len;
            return;
        }
    }
    
    // Handle string literals
    if (c == '"') {
        fmt->current_token.type = TOKEN_STRING;
        int start = fmt->input_pos;
        fmt->input_pos++; // Skip opening quote
        while (fmt->input_pos < fmt->input_size && fmt->input[fmt->input_pos] != '"') {
            if (fmt->input[fmt->input_pos] == '\\' && fmt->input_pos + 1 < fmt->input_size) {
                fmt->input_pos += 2; // Skip escape sequence
            } else {
                fmt->input_pos++;
            }
        }
        if (fmt->input_pos < fmt->input_size) {
            fmt->input_pos++; // Skip closing quote
        }
        int len = fmt->input_pos - start;
        strncpy(fmt->current_token.value, &fmt->input[start], len);
        fmt->current_token.value[len] = '\0';
        fmt->current_token.length = len;
        return;
    }
    
    // Handle identifiers and keywords
    if (isalpha(c) || c == '_') {
        fmt->current_token.type = TOKEN_IDENTIFIER;
        int start = fmt->input_pos;
        while (fmt->input_pos < fmt->input_size && 
               (isalnum(fmt->input[fmt->input_pos]) || fmt->input[fmt->input_pos] == '_')) {
            fmt->input_pos++;
        }
        int len = fmt->input_pos - start;
        strncpy(fmt->current_token.value, &fmt->input[start], len);
        fmt->current_token.value[len] = '\0';
        fmt->current_token.length = len;
        
        // Check if it's a keyword
        if (is_keyword(fmt->current_token.value)) {
            fmt->current_token.type = TOKEN_KEYWORD;
        }
        return;
    }
    
    // Handle numbers
    if (isdigit(c)) {
        fmt->current_token.type = TOKEN_NUMBER;
        int start = fmt->input_pos;
        while (fmt->input_pos < fmt->input_size && 
               (isdigit(fmt->input[fmt->input_pos]) || fmt->input[fmt->input_pos] == '.')) {
            fmt->input_pos++;
        }
        int len = fmt->input_pos - start;
        strncpy(fmt->current_token.value, &fmt->input[start], len);
        fmt->current_token.value[len] = '\0';
        fmt->current_token.length = len;
        return;
    }
    
    // Handle single character tokens
    fmt->current_token.value[0] = c;
    fmt->current_token.value[1] = '\0';
    fmt->current_token.length = 1;
    fmt->input_pos++;
    
    switch (c) {
        case '{': fmt->current_token.type = TOKEN_BRACE_OPEN; break;
        case '}': fmt->current_token.type = TOKEN_BRACE_CLOSE; break;
        case '(': fmt->current_token.type = TOKEN_PAREN_OPEN; break;
        case ')': fmt->current_token.type = TOKEN_PAREN_CLOSE; break;
        case '[': fmt->current_token.type = TOKEN_BRACKET_OPEN; break;
        case ']': fmt->current_token.type = TOKEN_BRACKET_CLOSE; break;
        case ';': fmt->current_token.type = TOKEN_SEMICOLON; break;
        case ',': fmt->current_token.type = TOKEN_COMMA; break;
        case '.': fmt->current_token.type = TOKEN_DOT; break;
        default: fmt->current_token.type = TOKEN_OPERATOR; break;
    }
}

int is_keyword(const char* word) {
    for (int i = 0; goo_keywords[i] != NULL; i++) {
        if (strcmp(word, goo_keywords[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

void emit_char(Formatter* fmt, char c) {
    if (fmt->output_size >= fmt->output_capacity - 1) {
        fmt->output_capacity *= 2;
        fmt->output = realloc(fmt->output, fmt->output_capacity);
    }
    fmt->output[fmt->output_size++] = c;
}

void emit_string(Formatter* fmt, const char* str) {
    size_t len = strlen(str);
    while (fmt->output_size + len >= fmt->output_capacity) {
        fmt->output_capacity *= 2;
        fmt->output = realloc(fmt->output, fmt->output_capacity);
    }
    memcpy(&fmt->output[fmt->output_size], str, len);
    fmt->output_size += len;
}

void emit_indent(Formatter* fmt) {
    for (int i = 0; i < fmt->indent_level; i++) {
        if (fmt->config.use_tabs) {
            emit_char(fmt, '\t');
        } else {
            for (int j = 0; j < fmt->config.indent_size; j++) {
                emit_char(fmt, ' ');
            }
        }
    }
}

void emit_newline(Formatter* fmt) {
    emit_char(fmt, '\n');
}

void emit_space(Formatter* fmt) {
    emit_char(fmt, ' ');
}

void format_tokens(Formatter* fmt) {
    int need_newline = 0;
    int suppress_space = 0;
    
    next_token(fmt);
    
    while (fmt->current_token.type != TOKEN_EOF) {
        switch (fmt->current_token.type) {
            case TOKEN_KEYWORD:
                if (need_newline) {
                    emit_newline(fmt);
                    emit_indent(fmt);
                    need_newline = 0;
                }
                
                // Handle special keywords
                if (strcmp(fmt->current_token.value, "func") == 0) {
                    fmt->in_function = 1;
                    if (fmt->config.blank_lines_around_functions && fmt->output_size > 1) {
                        emit_newline(fmt);
                    }
                } else if (strcmp(fmt->current_token.value, "struct") == 0 ||
                          strcmp(fmt->current_token.value, "interface") == 0) {
                    fmt->in_struct = 1;
                    if (fmt->config.blank_lines_around_types && fmt->output_size > 1) {
                        emit_newline(fmt);
                    }
                }
                
                emit_string(fmt, fmt->current_token.value);
                break;
                
            case TOKEN_IDENTIFIER:
            case TOKEN_NUMBER:
                if (need_newline) {
                    emit_newline(fmt);
                    emit_indent(fmt);
                    need_newline = 0;
                }
                emit_string(fmt, fmt->current_token.value);
                break;
                
            case TOKEN_STRING:
            case TOKEN_COMMENT_LINE:
            case TOKEN_COMMENT_BLOCK:
                if (need_newline) {
                    emit_newline(fmt);
                    emit_indent(fmt);
                    need_newline = 0;
                }
                emit_string(fmt, fmt->current_token.value);
                if (fmt->current_token.type == TOKEN_COMMENT_LINE) {
                    need_newline = 1;
                }
                break;
                
            case TOKEN_BRACE_OPEN:
                fmt->brace_depth++;
                if (fmt->config.space_before_brace && 
                    fmt->prev_token.type != TOKEN_EOF && 
                    !suppress_space) {
                    emit_space(fmt);
                }
                
                if (fmt->config.newline_before_brace) {
                    emit_newline(fmt);
                    emit_indent(fmt);
                }
                
                emit_char(fmt, '{');
                
                if (fmt->config.space_inside_brace) {
                    emit_space(fmt);
                }
                
                fmt->indent_level++;
                need_newline = 1;
                break;
                
            case TOKEN_BRACE_CLOSE:
                fmt->brace_depth--;
                fmt->indent_level--;
                
                if (fmt->config.space_inside_brace && 
                    fmt->prev_token.type != TOKEN_BRACE_OPEN) {
                    emit_space(fmt);
                }
                
                if (need_newline || fmt->prev_token.type != TOKEN_BRACE_OPEN) {
                    emit_newline(fmt);
                    emit_indent(fmt);
                }
                
                emit_char(fmt, '}');
                need_newline = 1;
                
                if (fmt->brace_depth == 0) {
                    fmt->in_function = 0;
                    fmt->in_struct = 0;
                    if (fmt->config.blank_lines_around_functions || 
                        fmt->config.blank_lines_around_types) {
                        emit_newline(fmt);
                    }
                }
                break;
                
            case TOKEN_PAREN_OPEN:
                fmt->paren_depth++;
                if (fmt->config.space_before_paren && 
                    fmt->prev_token.type == TOKEN_IDENTIFIER) {
                    emit_space(fmt);
                }
                emit_char(fmt, '(');
                if (fmt->config.space_inside_paren) {
                    emit_space(fmt);
                }
                break;
                
            case TOKEN_PAREN_CLOSE:
                fmt->paren_depth--;
                if (fmt->config.space_inside_paren && 
                    fmt->prev_token.type != TOKEN_PAREN_OPEN) {
                    emit_space(fmt);
                }
                emit_char(fmt, ')');
                break;
                
            case TOKEN_COMMA:
                emit_char(fmt, ',');
                if (fmt->config.space_after_comma) {
                    emit_space(fmt);
                }
                suppress_space = 1;
                break;
                
            case TOKEN_SEMICOLON:
                emit_char(fmt, ';');
                need_newline = 1;
                break;
                
            case TOKEN_OPERATOR:
                if (fmt->config.space_around_operators && !suppress_space) {
                    emit_space(fmt);
                }
                emit_string(fmt, fmt->current_token.value);
                if (fmt->config.space_around_operators) {
                    emit_space(fmt);
                    suppress_space = 1;
                }
                break;
                
            default:
                emit_string(fmt, fmt->current_token.value);
                break;
        }
        
        // Add space between tokens if needed
        next_token(fmt);
        if (!suppress_space && !need_newline && 
            fmt->current_token.type != TOKEN_EOF &&
            fmt->current_token.type != TOKEN_COMMA &&
            fmt->current_token.type != TOKEN_SEMICOLON &&
            fmt->current_token.type != TOKEN_BRACE_CLOSE &&
            fmt->current_token.type != TOKEN_PAREN_CLOSE &&
            fmt->current_token.type != TOKEN_BRACKET_CLOSE &&
            fmt->prev_token.type != TOKEN_BRACE_OPEN &&
            fmt->prev_token.type != TOKEN_PAREN_OPEN &&
            fmt->prev_token.type != TOKEN_BRACKET_OPEN &&
            fmt->prev_token.type != TOKEN_DOT &&
            fmt->current_token.type != TOKEN_DOT) {
            emit_space(fmt);
        }
        
        suppress_space = 0;
    }
}

int is_goo_file(const char* filename) {
    const char* dot = strrchr(filename, '.');
    return dot && strcmp(dot, ".goo") == 0;
}

void trim_whitespace(char* str) {
    char* end;
    
    // Trim leading space
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) return;
    
    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    end[1] = '\0';
}

int ensure_directory(const char* path) {
    char tmp[MAX_PATH_LENGTH];
    char* p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}