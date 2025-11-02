#ifndef DOCUMENTATION_GENERATOR_H
#define DOCUMENTATION_GENERATOR_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct ProjectHealthDashboard ProjectHealthDashboard;
typedef struct REPLContext REPLContext;

// =============================================================================
// Type Definitions
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
// Core Functions
// =============================================================================

// Lifecycle management
DocumentationGenerator* documentation_generator_new(const char* project_root);
void documentation_generator_free(DocumentationGenerator* gen);
int documentation_generator_init(DocumentationGenerator* gen);

// Documentation parsing
DocumentationComment* documentation_parse_comment(const char* comment_text);
DocumentationElement* documentation_parse_function(const char* source_line, 
                                                   const char* filename, 
                                                   int line_number,
                                                   DocumentationComment* comment);
DocumentationElement* documentation_parse_struct(const char* source_line,
                                                 const char* filename,
                                                 int line_number,
                                                 DocumentationComment* comment);
DocumentationElement* documentation_parse_interface(const char* source_line,
                                                    const char* filename,
                                                    int line_number,
                                                    DocumentationComment* comment);

// File processing
int documentation_generator_parse_file(DocumentationGenerator* gen, const char* filepath);
int documentation_generator_scan_directory(DocumentationGenerator* gen, const char* directory);

// Generation
int documentation_generator_generate(DocumentationGenerator* gen);
int documentation_generator_generate_html_index(DocumentationGenerator* gen);

// Helper functions
void documentation_element_free(DocumentationElement* element);
void documentation_section_free(DocumentationSection* section);

// Integration functions
int documentation_generator_integrate_health_dashboard(DocumentationGenerator* gen, ProjectHealthDashboard* dashboard);
int documentation_generator_integrate_repl(DocumentationGenerator* gen, REPLContext* repl);

#endif // DOCUMENTATION_GENERATOR_H