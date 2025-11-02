// Documentation Generator - Integrated documentation generation and preview
// Provides comprehensive documentation generation for the Goo development environment

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

// Forward declarations for integration
typedef struct ProjectHealthDashboard ProjectHealthDashboard;
typedef struct REPLContext REPLContext;

// =============================================================================
// Data Structures
// =============================================================================

// Documentation formats
typedef enum {
    DOC_FORMAT_HTML,
    DOC_FORMAT_MARKDOWN,
    DOC_FORMAT_PDF,
    DOC_FORMAT_JSON,
    DOC_FORMAT_PLAIN_TEXT
} DocumentationFormat;

// Documentation element types
typedef enum {
    DOC_ELEMENT_FUNCTION,
    DOC_ELEMENT_STRUCT,
    DOC_ELEMENT_ENUM,
    DOC_ELEMENT_INTERFACE,
    DOC_ELEMENT_CONSTANT,
    DOC_ELEMENT_VARIABLE,
    DOC_ELEMENT_MODULE,
    DOC_ELEMENT_PACKAGE,
    DOC_ELEMENT_TYPE_ALIAS,
    DOC_ELEMENT_CONCEPT
} DocumentationElementType;

// Documentation comment
typedef struct DocumentationComment {
    char* brief_description;
    char* detailed_description;
    char** parameters;
    char** parameter_descriptions;
    int parameter_count;
    char* return_description;
    char** examples;
    int example_count;
    char** see_also;
    int see_also_count;
    char* since_version;
    char* deprecated_message;
    bool is_deprecated;
    char** tags;
    int tag_count;
} DocumentationComment;

// Documentation element
typedef struct DocumentationElement {
    char* name;
    char* qualified_name;
    char* signature;
    char* source_file;
    int line_number;
    DocumentationElementType type;
    DocumentationComment* comment;
    char* visibility; // public, private, package
    char* module_name;
    char* package_name;
    struct DocumentationElement* next;
} DocumentationElement;

// Documentation section
typedef struct DocumentationSection {
    char* title;
    char* content;
    DocumentationElement* elements;
    int element_count;
    struct DocumentationSection* subsections;
    struct DocumentationSection* next;
} DocumentationSection;

// Documentation configuration
typedef struct DocumentationConfig {
    DocumentationFormat output_format;
    char* output_directory;
    char* template_directory;
    bool include_private;
    bool include_internal;
    bool generate_index;
    bool generate_search;
    bool include_source_links;
    bool include_examples;
    bool live_preview;
    int preview_port;
    char* project_name;
    char* project_version;
    char* project_description;
    char* author;
    char* custom_css;
    char* custom_header;
    char* custom_footer;
} DocumentationConfig;

// Documentation generator
typedef struct DocumentationGenerator {
    bool is_enabled;
    bool is_generating;
    char* project_root;
    
    // Elements and sections
    DocumentationElement* elements;
    DocumentationSection* sections;
    int total_elements;
    int processed_files;
    
    // Configuration
    DocumentationConfig config;
    
    // Statistics
    uint64_t generation_start_time;
    uint64_t generation_end_time;
    int functions_documented;
    int structs_documented;
    int interfaces_documented;
    int modules_documented;
    
    // Integration
    ProjectHealthDashboard* health_dashboard;
    REPLContext* repl;
    
} DocumentationGenerator;

// =============================================================================
// Color Constants
// =============================================================================

#define DOC_COLOR_RESET   "\033[0m"
#define DOC_COLOR_BOLD    "\033[1m"
#define DOC_COLOR_RED     "\033[31m"
#define DOC_COLOR_GREEN   "\033[32m"
#define DOC_COLOR_YELLOW  "\033[33m"
#define DOC_COLOR_BLUE    "\033[34m"
#define DOC_COLOR_MAGENTA "\033[35m"
#define DOC_COLOR_CYAN    "\033[36m"
#define DOC_COLOR_WHITE   "\033[37m"

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t doc_get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static char* doc_duplicate_string(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

static void doc_print_colored(const char* color, const char* format, ...) {
    printf("%s", color);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("%s", DOC_COLOR_RESET);
}

static void create_directory_if_not_exists(const char* path) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    }
    mkdir(tmp, S_IRWXU);
}

static bool is_goo_source_file(const char* filename) {
    if (!filename) return false;
    size_t len = strlen(filename);
    return len >= 4 && strcmp(filename + len - 4, ".goo") == 0;
}

// =============================================================================
// Documentation Generator Lifecycle
// =============================================================================

DocumentationGenerator* documentation_generator_new(const char* project_root) {
    DocumentationGenerator* gen = calloc(1, sizeof(DocumentationGenerator));
    if (!gen) return NULL;
    
    gen->project_root = doc_duplicate_string(project_root ? project_root : ".");
    gen->is_enabled = true;
    gen->is_generating = false;
    
    // Initialize configuration with defaults
    gen->config.output_format = DOC_FORMAT_HTML;
    gen->config.output_directory = doc_duplicate_string("./docs");
    gen->config.template_directory = doc_duplicate_string("./templates/docs");
    gen->config.include_private = false;
    gen->config.include_internal = false;
    gen->config.generate_index = true;
    gen->config.generate_search = true;
    gen->config.include_source_links = true;
    gen->config.include_examples = true;
    gen->config.live_preview = false;
    gen->config.preview_port = 8080;
    gen->config.project_name = doc_duplicate_string("Goo Project");
    gen->config.project_version = doc_duplicate_string("1.0.0");
    gen->config.project_description = doc_duplicate_string("A Goo programming language project");
    gen->config.author = doc_duplicate_string("Goo Developer");
    
    return gen;
}

void documentation_generator_free(DocumentationGenerator* gen) {
    if (!gen) return;
    
    // Free elements
    DocumentationElement* element = gen->elements;
    while (element) {
        DocumentationElement* next = element->next;
        documentation_element_free(element);
        element = next;
    }
    
    // Free sections
    DocumentationSection* section = gen->sections;
    while (section) {
        DocumentationSection* next = section->next;
        documentation_section_free(section);
        section = next;
    }
    
    // Free configuration strings
    free(gen->config.output_directory);
    free(gen->config.template_directory);
    free(gen->config.project_name);
    free(gen->config.project_version);
    free(gen->config.project_description);
    free(gen->config.author);
    free(gen->config.custom_css);
    free(gen->config.custom_header);
    free(gen->config.custom_footer);
    
    free(gen->project_root);
    free(gen);
}

// Forward declarations for helper functions
void documentation_element_free(DocumentationElement* element);
void documentation_section_free(DocumentationSection* section);

int documentation_generator_init(DocumentationGenerator* gen) {
    if (!gen) return -1;
    
    // Create output directories
    create_directory_if_not_exists(gen->config.output_directory);
    
    char assets_dir[512];
    snprintf(assets_dir, sizeof(assets_dir), "%s/assets", gen->config.output_directory);
    create_directory_if_not_exists(assets_dir);
    
    gen->is_enabled = true;
    return 0;
}

// =============================================================================
// Documentation Comment Parsing
// =============================================================================

DocumentationComment* documentation_parse_comment(const char* comment_text) {
    if (!comment_text) return NULL;
    
    DocumentationComment* comment = calloc(1, sizeof(DocumentationComment));
    if (!comment) return NULL;
    
    // Simple parser for Goo-style documentation comments
    // Format: /// Brief description
    //         /// Detailed description
    //         /// @param name Description
    //         /// @return Description
    //         /// @example code_example
    
    char* text_copy = doc_duplicate_string(comment_text);
    char* line = strtok(text_copy, "\n");
    
    bool in_detailed = false;
    char detailed_buffer[4096] = {0};
    
    while (line) {
        // Skip leading whitespace and comment markers
        while (*line && (isspace(*line) || *line == '/' || *line == '*')) {
            line++;
        }
        
        if (strlen(line) == 0) {
            line = strtok(NULL, "\n");
            continue;
        }
        
        // Parse tags
        if (strncmp(line, "@param", 6) == 0) {
            char* param_line = line + 6;
            while (isspace(*param_line)) param_line++;
            
            char* space = strchr(param_line, ' ');
            if (space) {
                *space = '\0';
                char* param_name = doc_duplicate_string(param_line);
                char* param_desc = doc_duplicate_string(space + 1);
                
                // Add to parameters array
                comment->parameters = realloc(comment->parameters, 
                                            (comment->parameter_count + 1) * sizeof(char*));
                comment->parameter_descriptions = realloc(comment->parameter_descriptions,
                                                        (comment->parameter_count + 1) * sizeof(char*));
                comment->parameters[comment->parameter_count] = param_name;
                comment->parameter_descriptions[comment->parameter_count] = param_desc;
                comment->parameter_count++;
            }
        } else if (strncmp(line, "@return", 7) == 0) {
            char* return_line = line + 7;
            while (isspace(*return_line)) return_line++;
            comment->return_description = doc_duplicate_string(return_line);
        } else if (strncmp(line, "@example", 8) == 0) {
            char* example_line = line + 8;
            while (isspace(*example_line)) example_line++;
            
            comment->examples = realloc(comment->examples,
                                      (comment->example_count + 1) * sizeof(char*));
            comment->examples[comment->example_count] = doc_duplicate_string(example_line);
            comment->example_count++;
        } else if (strncmp(line, "@since", 6) == 0) {
            char* since_line = line + 6;
            while (isspace(*since_line)) since_line++;
            comment->since_version = doc_duplicate_string(since_line);
        } else if (strncmp(line, "@deprecated", 11) == 0) {
            comment->is_deprecated = true;
            char* deprecated_line = line + 11;
            while (isspace(*deprecated_line)) deprecated_line++;
            if (strlen(deprecated_line) > 0) {
                comment->deprecated_message = doc_duplicate_string(deprecated_line);
            }
        } else {
            // Regular description text
            if (!comment->brief_description) {
                comment->brief_description = doc_duplicate_string(line);
                in_detailed = true;
            } else if (in_detailed) {
                if (strlen(detailed_buffer) > 0) {
                    strcat(detailed_buffer, "\n");
                }
                strcat(detailed_buffer, line);
            }
        }
        
        line = strtok(NULL, "\n");
    }
    
    if (strlen(detailed_buffer) > 0) {
        comment->detailed_description = doc_duplicate_string(detailed_buffer);
    }
    
    free(text_copy);
    return comment;
}

// =============================================================================
// Source Code Analysis
// =============================================================================

DocumentationElement* documentation_parse_function(const char* source_line, 
                                                   const char* filename, 
                                                   int line_number,
                                                   DocumentationComment* comment) {
    if (!source_line || strncmp(source_line, "fn ", 3) != 0) return NULL;
    
    DocumentationElement* element = calloc(1, sizeof(DocumentationElement));
    if (!element) return NULL;
    
    element->type = DOC_ELEMENT_FUNCTION;
    element->source_file = doc_duplicate_string(filename);
    element->line_number = line_number;
    element->comment = comment;
    element->visibility = doc_duplicate_string("public"); // Default
    
    // Extract function name and signature
    char* func_start = (char*)source_line + 3; // Skip "fn "
    char* paren = strchr(func_start, '(');
    if (paren) {
        *paren = '\0';
        element->name = doc_duplicate_string(func_start);
        *paren = '(';
        
        // Find the end of the function signature
        char* brace = strchr(paren, '{');
        if (brace) {
            size_t sig_len = brace - func_start;
            char* signature = malloc(sig_len + 1);
            strncpy(signature, func_start, sig_len);
            signature[sig_len] = '\0';
            element->signature = signature;
        } else {
            element->signature = doc_duplicate_string(func_start);
        }
    }
    
    return element;
}

DocumentationElement* documentation_parse_struct(const char* source_line,
                                                 const char* filename,
                                                 int line_number,
                                                 DocumentationComment* comment) {
    if (!source_line || strncmp(source_line, "struct ", 7) != 0) return NULL;
    
    DocumentationElement* element = calloc(1, sizeof(DocumentationElement));
    if (!element) return NULL;
    
    element->type = DOC_ELEMENT_STRUCT;
    element->source_file = doc_duplicate_string(filename);
    element->line_number = line_number;
    element->comment = comment;
    element->visibility = doc_duplicate_string("public");
    
    // Extract struct name
    char* struct_start = (char*)source_line + 7; // Skip "struct "
    char* space = strchr(struct_start, ' ');
    char* brace = strchr(struct_start, '{');
    char* end = space < brace && space ? space : brace;
    
    if (end) {
        size_t name_len = end - struct_start;
        char* name = malloc(name_len + 1);
        strncpy(name, struct_start, name_len);
        name[name_len] = '\0';
        element->name = name;
        element->signature = doc_duplicate_string(source_line);
    }
    
    return element;
}

DocumentationElement* documentation_parse_interface(const char* source_line,
                                                    const char* filename,
                                                    int line_number,
                                                    DocumentationComment* comment) {
    if (!source_line || strncmp(source_line, "interface ", 10) != 0) return NULL;
    
    DocumentationElement* element = calloc(1, sizeof(DocumentationElement));
    if (!element) return NULL;
    
    element->type = DOC_ELEMENT_INTERFACE;
    element->source_file = doc_duplicate_string(filename);
    element->line_number = line_number;
    element->comment = comment;
    element->visibility = doc_duplicate_string("public");
    
    // Extract interface name
    char* interface_start = (char*)source_line + 10; // Skip "interface "
    char* space = strchr(interface_start, ' ');
    char* brace = strchr(interface_start, '{');
    char* end = space < brace && space ? space : brace;
    
    if (end) {
        size_t name_len = end - interface_start;
        char* name = malloc(name_len + 1);
        strncpy(name, interface_start, name_len);
        name[name_len] = '\0';
        element->name = name;
        element->signature = doc_duplicate_string(source_line);
    }
    
    return element;
}

int documentation_generator_parse_file(DocumentationGenerator* gen, const char* filepath) {
    if (!gen || !filepath) return -1;
    
    FILE* file = fopen(filepath, "r");
    if (!file) return -1;
    
    char line[1024];
    int line_number = 0;
    char comment_buffer[4096] = {0};
    bool in_comment_block = false;
    
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        
        // Remove trailing newline
        line[strcspn(line, "\n")] = 0;
        
        // Check for documentation comments
        char* trimmed = line;
        while (isspace(*trimmed)) trimmed++;
        
        if (strncmp(trimmed, "///", 3) == 0) {
            // Documentation comment
            if (strlen(comment_buffer) > 0) {
                strcat(comment_buffer, "\n");
            }
            strcat(comment_buffer, trimmed);
            in_comment_block = true;
        } else if (in_comment_block && strlen(trimmed) > 0) {
            // We have a comment block, now parse the following code
            DocumentationComment* comment = documentation_parse_comment(comment_buffer);
            DocumentationElement* element = NULL;
            
            if (strncmp(trimmed, "fn ", 3) == 0) {
                element = documentation_parse_function(trimmed, filepath, line_number, comment);
                gen->functions_documented++;
            } else if (strncmp(trimmed, "struct ", 7) == 0) {
                element = documentation_parse_struct(trimmed, filepath, line_number, comment);
                gen->structs_documented++;
            } else if (strncmp(trimmed, "interface ", 10) == 0) {
                element = documentation_parse_interface(trimmed, filepath, line_number, comment);
                gen->interfaces_documented++;
            }
            
            if (element) {
                // Add to elements list
                element->next = gen->elements;
                gen->elements = element;
                gen->total_elements++;
            }
            
            // Reset comment state
            comment_buffer[0] = '\0';
            in_comment_block = false;
        } else if (strlen(trimmed) > 0) {
            // Non-empty line, reset comment state
            comment_buffer[0] = '\0';
            in_comment_block = false;
        }
    }
    
    fclose(file);
    gen->processed_files++;
    return 0;
}

int documentation_generator_scan_directory(DocumentationGenerator* gen, const char* directory) {
    if (!gen || !directory) return -1;
    
    DIR* dir = opendir(directory);
    if (!dir) return -1;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->d_name);
        
        struct stat statbuf;
        if (stat(full_path, &statbuf) != 0) continue;
        
        if (S_ISDIR(statbuf.st_mode)) {
            // Recursively scan subdirectories
            documentation_generator_scan_directory(gen, full_path);
        } else if (S_ISREG(statbuf.st_mode) && is_goo_source_file(entry->d_name)) {
            // Parse source file
            documentation_generator_parse_file(gen, full_path);
        }
    }
    
    closedir(dir);
    return 0;
}

// =============================================================================
// HTML Generation
// =============================================================================

int documentation_generator_generate_html_index(DocumentationGenerator* gen) {
    if (!gen) return -1;
    
    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/index.html", gen->config.output_directory);
    
    FILE* html = fopen(index_path, "w");
    if (!html) return -1;
    
    // HTML header
    fprintf(html, "<!DOCTYPE html>\n");
    fprintf(html, "<html lang=\"en\">\n");
    fprintf(html, "<head>\n");
    fprintf(html, "    <meta charset=\"UTF-8\">\n");
    fprintf(html, "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(html, "    <title>%s Documentation</title>\n", gen->config.project_name);
    fprintf(html, "    <style>\n");
    fprintf(html, "        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; margin: 0; padding: 0; line-height: 1.6; }\n");
    fprintf(html, "        .header { background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); color: white; padding: 40px 20px; text-align: center; }\n");
    fprintf(html, "        .container { max-width: 1200px; margin: 0 auto; padding: 20px; }\n");
    fprintf(html, "        .nav { background: #f8f9fa; padding: 15px; border-radius: 8px; margin-bottom: 30px; }\n");
    fprintf(html, "        .nav a { text-decoration: none; color: #667eea; margin-right: 20px; font-weight: 500; }\n");
    fprintf(html, "        .nav a:hover { color: #764ba2; }\n");
    fprintf(html, "        .section { background: white; border-radius: 8px; padding: 30px; margin-bottom: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n");
    fprintf(html, "        .element-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; }\n");
    fprintf(html, "        .element-card { border: 1px solid #e9ecef; border-radius: 8px; padding: 20px; transition: all 0.3s ease; }\n");
    fprintf(html, "        .element-card:hover { box-shadow: 0 4px 15px rgba(0,0,0,0.1); transform: translateY(-2px); }\n");
    fprintf(html, "        .element-title { font-size: 1.2em; font-weight: bold; color: #333; margin-bottom: 10px; }\n");
    fprintf(html, "        .element-type { background: #667eea; color: white; padding: 3px 8px; border-radius: 12px; font-size: 0.8em; }\n");
    fprintf(html, "        .element-description { color: #666; margin-top: 10px; }\n");
    fprintf(html, "        .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 20px; margin-bottom: 30px; }\n");
    fprintf(html, "        .stat-card { text-align: center; background: #f8f9fa; padding: 20px; border-radius: 8px; }\n");
    fprintf(html, "        .stat-value { font-size: 2em; font-weight: bold; color: #667eea; }\n");
    fprintf(html, "        .stat-label { color: #666; margin-top: 5px; }\n");
    fprintf(html, "        .search-box { width: 100%%; padding: 12px; border: 1px solid #ddd; border-radius: 8px; font-size: 16px; margin-bottom: 20px; }\n");
    fprintf(html, "    </style>\n");
    fprintf(html, "</head>\n");
    fprintf(html, "<body>\n");
    
    // Header
    fprintf(html, "    <div class=\"header\">\n");
    fprintf(html, "        <h1>📚 %s Documentation</h1>\n", gen->config.project_name);
    fprintf(html, "        <p>%s</p>\n", gen->config.project_description);
    fprintf(html, "        <p>Version %s | Generated on %s</p>\n", 
            gen->config.project_version, __DATE__);
    fprintf(html, "    </div>\n");
    
    fprintf(html, "    <div class=\"container\">\n");
    
    // Navigation
    fprintf(html, "        <div class=\"nav\">\n");
    fprintf(html, "            <a href=\"#overview\">📋 Overview</a>\n");
    fprintf(html, "            <a href=\"#functions\">🔧 Functions</a>\n");
    fprintf(html, "            <a href=\"#structures\">🏗️ Structures</a>\n");
    fprintf(html, "            <a href=\"#interfaces\">🔌 Interfaces</a>\n");
    fprintf(html, "            <a href=\"#modules\">📦 Modules</a>\n");
    fprintf(html, "        </div>\n");
    
    // Search
    if (gen->config.generate_search) {
        fprintf(html, "        <input type=\"text\" class=\"search-box\" placeholder=\"🔍 Search documentation...\" id=\"searchBox\">\n");
    }
    
    // Statistics
    fprintf(html, "        <div class=\"stats\">\n");
    fprintf(html, "            <div class=\"stat-card\">\n");
    fprintf(html, "                <div class=\"stat-value\">%d</div>\n", gen->functions_documented);
    fprintf(html, "                <div class=\"stat-label\">Functions</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "            <div class=\"stat-card\">\n");
    fprintf(html, "                <div class=\"stat-value\">%d</div>\n", gen->structs_documented);
    fprintf(html, "                <div class=\"stat-label\">Structures</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "            <div class=\"stat-card\">\n");
    fprintf(html, "                <div class=\"stat-value\">%d</div>\n", gen->interfaces_documented);
    fprintf(html, "                <div class=\"stat-label\">Interfaces</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "            <div class=\"stat-card\">\n");
    fprintf(html, "                <div class=\"stat-value\">%d</div>\n", gen->processed_files);
    fprintf(html, "                <div class=\"stat-label\">Source Files</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "        </div>\n");
    
    // Functions section
    fprintf(html, "        <div class=\"section\" id=\"functions\">\n");
    fprintf(html, "            <h2>🔧 Functions</h2>\n");
    fprintf(html, "            <div class=\"element-grid\">\n");
    
    DocumentationElement* element = gen->elements;
    while (element) {
        if (element->type == DOC_ELEMENT_FUNCTION) {
            fprintf(html, "                <div class=\"element-card\">\n");
            fprintf(html, "                    <div class=\"element-title\">\n");
            fprintf(html, "                        <span class=\"element-type\">function</span>\n");
            fprintf(html, "                        %s\n", element->name ? element->name : "unnamed");
            fprintf(html, "                    </div>\n");
            if (element->signature) {
                fprintf(html, "                    <code>%s</code>\n", element->signature);
            }
            if (element->comment && element->comment->brief_description) {
                fprintf(html, "                    <div class=\"element-description\">%s</div>\n", 
                        element->comment->brief_description);
            }
            if (gen->config.include_source_links && element->source_file) {
                fprintf(html, "                    <small>📄 %s:%d</small>\n", 
                        element->source_file, element->line_number);
            }
            fprintf(html, "                </div>\n");
        }
        element = element->next;
    }
    
    fprintf(html, "            </div>\n");
    fprintf(html, "        </div>\n");
    
    // Structures section
    fprintf(html, "        <div class=\"section\" id=\"structures\">\n");
    fprintf(html, "            <h2>🏗️ Structures</h2>\n");
    fprintf(html, "            <div class=\"element-grid\">\n");
    
    element = gen->elements;
    while (element) {
        if (element->type == DOC_ELEMENT_STRUCT) {
            fprintf(html, "                <div class=\"element-card\">\n");
            fprintf(html, "                    <div class=\"element-title\">\n");
            fprintf(html, "                        <span class=\"element-type\">struct</span>\n");
            fprintf(html, "                        %s\n", element->name ? element->name : "unnamed");
            fprintf(html, "                    </div>\n");
            if (element->signature) {
                fprintf(html, "                    <code>%s</code>\n", element->signature);
            }
            if (element->comment && element->comment->brief_description) {
                fprintf(html, "                    <div class=\"element-description\">%s</div>\n", 
                        element->comment->brief_description);
            }
            if (gen->config.include_source_links && element->source_file) {
                fprintf(html, "                    <small>📄 %s:%d</small>\n", 
                        element->source_file, element->line_number);
            }
            fprintf(html, "                </div>\n");
        }
        element = element->next;
    }
    
    fprintf(html, "            </div>\n");
    fprintf(html, "        </div>\n");
    
    // Interfaces section
    fprintf(html, "        <div class=\"section\" id=\"interfaces\">\n");
    fprintf(html, "            <h2>🔌 Interfaces</h2>\n");
    fprintf(html, "            <div class=\"element-grid\">\n");
    
    element = gen->elements;
    while (element) {
        if (element->type == DOC_ELEMENT_INTERFACE) {
            fprintf(html, "                <div class=\"element-card\">\n");
            fprintf(html, "                    <div class=\"element-title\">\n");
            fprintf(html, "                        <span class=\"element-type\">interface</span>\n");
            fprintf(html, "                        %s\n", element->name ? element->name : "unnamed");
            fprintf(html, "                    </div>\n");
            if (element->signature) {
                fprintf(html, "                    <code>%s</code>\n", element->signature);
            }
            if (element->comment && element->comment->brief_description) {
                fprintf(html, "                    <div class=\"element-description\">%s</div>\n", 
                        element->comment->brief_description);
            }
            if (gen->config.include_source_links && element->source_file) {
                fprintf(html, "                    <small>📄 %s:%d</small>\n", 
                        element->source_file, element->line_number);
            }
            fprintf(html, "                </div>\n");
        }
        element = element->next;
    }
    
    fprintf(html, "            </div>\n");
    fprintf(html, "        </div>\n");
    
    // Footer
    fprintf(html, "    </div>\n");
    
    // Search JavaScript
    if (gen->config.generate_search) {
        fprintf(html, "    <script>\n");
        fprintf(html, "        const searchBox = document.getElementById('searchBox');\n");
        fprintf(html, "        const cards = document.querySelectorAll('.element-card');\n");
        fprintf(html, "        \n");
        fprintf(html, "        searchBox.addEventListener('input', function() {\n");
        fprintf(html, "            const query = this.value.toLowerCase();\n");
        fprintf(html, "            cards.forEach(card => {\n");
        fprintf(html, "                const text = card.textContent.toLowerCase();\n");
        fprintf(html, "                card.style.display = text.includes(query) ? 'block' : 'none';\n");
        fprintf(html, "            });\n");
        fprintf(html, "        });\n");
        fprintf(html, "    </script>\n");
    }
    
    fprintf(html, "</body>\n");
    fprintf(html, "</html>\n");
    
    fclose(html);
    return 0;
}

// =============================================================================
// Main Documentation Generation
// =============================================================================

int documentation_generator_generate(DocumentationGenerator* gen) {
    if (!gen) return -1;
    
    gen->is_generating = true;
    gen->generation_start_time = doc_get_timestamp_ms();
    
    doc_print_colored(DOC_COLOR_BOLD DOC_COLOR_CYAN, "📚 Generating Documentation\n");
    doc_print_colored(DOC_COLOR_CYAN, "============================\n");
    
    // Initialize counters
    gen->functions_documented = 0;
    gen->structs_documented = 0;
    gen->interfaces_documented = 0;
    gen->modules_documented = 0;
    gen->processed_files = 0;
    gen->total_elements = 0;
    
    // Scan source files
    doc_print_colored(DOC_COLOR_BLUE, "🔍 Scanning source files...\n");
    documentation_generator_scan_directory(gen, gen->project_root);
    
    doc_print_colored(DOC_COLOR_GREEN, "✅ Processed %d files, found %d elements\n", 
                     gen->processed_files, gen->total_elements);
    
    // Generate documentation based on format
    switch (gen->config.output_format) {
        case DOC_FORMAT_HTML:
            doc_print_colored(DOC_COLOR_BLUE, "🌐 Generating HTML documentation...\n");
            documentation_generator_generate_html_index(gen);
            break;
        case DOC_FORMAT_MARKDOWN:
            doc_print_colored(DOC_COLOR_BLUE, "📝 Generating Markdown documentation...\n");
            // TODO: Implement markdown generation
            break;
        default:
            doc_print_colored(DOC_COLOR_RED, "❌ Unsupported format\n");
            return -1;
    }
    
    gen->generation_end_time = doc_get_timestamp_ms();
    double generation_time = (gen->generation_end_time - gen->generation_start_time) / 1000.0;
    
    doc_print_colored(DOC_COLOR_GREEN, "🎉 Documentation generated successfully!\n");
    doc_print_colored(DOC_COLOR_CYAN, "📊 Statistics:\n");
    printf("   Functions:   %d\n", gen->functions_documented);
    printf("   Structures:  %d\n", gen->structs_documented);
    printf("   Interfaces:  %d\n", gen->interfaces_documented);
    printf("   Files:       %d\n", gen->processed_files);
    printf("   Time:        %.2f seconds\n", generation_time);
    printf("   Output:      %s\n", gen->config.output_directory);
    
    gen->is_generating = false;
    return 0;
}

// =============================================================================
// Helper Functions (implementations for forward declarations)
// =============================================================================

void documentation_element_free(DocumentationElement* element) {
    if (!element) return;
    
    free(element->name);
    free(element->qualified_name);
    free(element->signature);
    free(element->source_file);
    free(element->visibility);
    free(element->module_name);
    free(element->package_name);
    
    if (element->comment) {
        free(element->comment->brief_description);
        free(element->comment->detailed_description);
        free(element->comment->return_description);
        free(element->comment->since_version);
        free(element->comment->deprecated_message);
        
        for (int i = 0; i < element->comment->parameter_count; i++) {
            free(element->comment->parameters[i]);
            free(element->comment->parameter_descriptions[i]);
        }
        free(element->comment->parameters);
        free(element->comment->parameter_descriptions);
        
        for (int i = 0; i < element->comment->example_count; i++) {
            free(element->comment->examples[i]);
        }
        free(element->comment->examples);
        
        for (int i = 0; i < element->comment->see_also_count; i++) {
            free(element->comment->see_also[i]);
        }
        free(element->comment->see_also);
        
        for (int i = 0; i < element->comment->tag_count; i++) {
            free(element->comment->tags[i]);
        }
        free(element->comment->tags);
        
        free(element->comment);
    }
    
    free(element);
}

void documentation_section_free(DocumentationSection* section) {
    if (!section) return;
    
    free(section->title);
    free(section->content);
    
    DocumentationElement* element = section->elements;
    while (element) {
        DocumentationElement* next = element->next;
        documentation_element_free(element);
        element = next;
    }
    
    DocumentationSection* subsection = section->subsections;
    while (subsection) {
        DocumentationSection* next = subsection->next;
        documentation_section_free(subsection);
        subsection = next;
    }
    
    free(section);
}

// =============================================================================
// Integration Functions
// =============================================================================

int documentation_generator_integrate_health_dashboard(DocumentationGenerator* gen, ProjectHealthDashboard* dashboard) {
    if (!gen) return -1;
    gen->health_dashboard = dashboard;
    return 0;
}

int documentation_generator_integrate_repl(DocumentationGenerator* gen, REPLContext* repl) {
    if (!gen) return -1;
    gen->repl = repl;
    return 0;
}