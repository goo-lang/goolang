#ifndef GOO_ERROR_HIERARCHIES_H
#define GOO_ERROR_HIERARCHIES_H

#include "errors/error.h"
#include "runtime.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Structured Error Hierarchies System
// =============================================================================

// Forward declarations
typedef struct ErrorTypeDefinition ErrorTypeDefinition;
typedef struct ErrorHierarchy ErrorHierarchy;
typedef struct StructuredError StructuredError;
typedef struct ErrorVariant ErrorVariant;
typedef struct ErrorField ErrorField;
typedef struct ErrorMatchPattern ErrorMatchPattern;

// =============================================================================
// Error Type System
// =============================================================================

// Error field types for structured error data
typedef enum {
    ERROR_FIELD_STRING,     // String value
    ERROR_FIELD_INT,        // Integer value  
    ERROR_FIELD_FLOAT,      // Float value
    ERROR_FIELD_BOOL,       // Boolean value
    ERROR_FIELD_ENUM,       // Enumerated value
    ERROR_FIELD_STRUCT,     // Nested structured data
    ERROR_FIELD_ARRAY,      // Array of values
    ERROR_FIELD_OPTIONAL    // Optional field (nullable)
} ErrorFieldType;

// Error field definition
typedef struct ErrorField {
    const char* name;           // Field name
    ErrorFieldType type;        // Field type
    const char* description;    // Human-readable description
    bool required;              // Whether field is required
    
    // Type-specific constraints
    union {
        struct {                // For string fields
            int min_length;
            int max_length;
            const char* pattern;    // Regex pattern (optional)
        } string_constraints;
        
        struct {                // For numeric fields
            int64_t min_value;
            int64_t max_value;
        } numeric_constraints;
        
        struct {                // For enum fields
            const char** valid_values;
            int value_count;
        } enum_constraints;
        
        struct {                // For array fields
            ErrorFieldType element_type;
            int min_elements;
            int max_elements;
        } array_constraints;
    };
    
    // Default value (optional)
    union {
        const char* string_default;
        int64_t int_default;
        double float_default;
        bool bool_default;
    } default_value;
    
} ErrorField;

// Error variant (like enum variants with data)
typedef struct ErrorVariant {
    const char* name;           // Variant name (e.g., "FileNotFound")
    const char* description;    // Human-readable description
    int error_code;             // Unique error code
    ErrorSeverity severity;     // Default severity
    
    // Fields for this variant
    ErrorField* fields;         // Array of fields
    int field_count;           // Number of fields
    
    // Formatting and localization
    const char* format_template; // Template for error messages
    const char* hint_template;   // Template for hints
    
    // Recovery suggestions
    const char** recovery_actions; // Array of suggested actions
    int recovery_action_count;
    
} ErrorVariant;

// Error type definition (like an error enum with structured data)
typedef struct ErrorTypeDefinition {
    const char* name;           // Type name (e.g., "ConfigError")
    const char* description;    // Description of error type
    const char* namespace;      // Namespace/module name
    
    // Variants
    ErrorVariant* variants;     // Array of error variants
    int variant_count;         // Number of variants
    
    // Inheritance
    ErrorTypeDefinition* parent; // Parent error type (for inheritance)
    ErrorTypeDefinition** children; // Child error types
    int child_count;           // Number of child types
    
    // Metadata
    uint64_t type_id;          // Unique type identifier
    const char* source_file;   // Where this type was defined
    int source_line;           // Line number in source
    
    // Runtime information
    bool is_abstract;          // Whether this is an abstract base type
    void (*type_destructor)(StructuredError*); // Custom destructor
    
} ErrorTypeDefinition;

// =============================================================================
// Structured Error Instance
// =============================================================================

// Error field value
typedef struct ErrorFieldValue {
    const char* field_name;     // Name of the field
    ErrorFieldType type;        // Type of the value
    
    // Value storage
    union {
        char* string_value;
        int64_t int_value;
        double float_value;
        bool bool_value;
        struct {
            void* data;
            int element_count;
        } array_value;
        struct ErrorFieldValue* struct_value; // For nested structures
    };
    
    // Metadata
    bool is_set;               // Whether this field has been set
    bool owns_memory;          // Whether we own the memory for this value
    
} ErrorFieldValue;

// Structured error instance
typedef struct StructuredError {
    // Type information
    ErrorTypeDefinition* type_def;  // Error type definition
    ErrorVariant* variant;          // Specific variant of this error
    
    // Field values
    ErrorFieldValue* field_values;  // Array of field values
    int field_count;               // Number of fields
    
    // Standard error information
    const char* message;           // Generated error message
    const char* hint;              // Generated hint
    ErrorSeverity severity;        // Error severity
    SourceLocation location;       // Source location
    
    // Error chaining
    StructuredError* cause;        // Caused by another structured error
    goo_error_t* root_cause;       // Original runtime error (if any)
    
    // Context and metadata
    const char* operation_context; // What operation was being performed
    uint64_t timestamp_ms;         // When error occurred
    uint64_t thread_id;            // Thread that created the error
    
    // Memory management
    bool owns_field_memory;        // Whether we own field memory
    void (*destructor)(StructuredError*); // Custom destructor
    
} StructuredError;

// =============================================================================
// Error Hierarchy Management
// =============================================================================

// Error hierarchy registry
typedef struct ErrorHierarchy {
    // Type registry
    ErrorTypeDefinition** registered_types;  // Array of registered types
    int type_count;                          // Number of registered types
    int type_capacity;                       // Capacity of types array
    
    // Type lookup tables
    struct {
        uint64_t* type_ids;                  // Array of type IDs
        ErrorTypeDefinition** type_defs;     // Corresponding type definitions
        int lookup_count;                    // Number of entries
    } type_lookup;
    
    // Inheritance relationships
    struct {
        ErrorTypeDefinition* parent;
        ErrorTypeDefinition** children;
        int child_count;
    }* inheritance_table;
    int inheritance_count;
    
    // Statistics
    struct {
        uint64_t errors_created;
        uint64_t errors_matched;
        uint64_t type_checks_performed;
        uint64_t inheritance_queries;
    } stats;
    
} ErrorHierarchy;

// =============================================================================
// Error Type Registration and Management
// =============================================================================

// Initialize the error hierarchy system
void error_hierarchy_system_init(void);
void error_hierarchy_system_shutdown(void);

// Get the global error hierarchy
ErrorHierarchy* get_global_error_hierarchy(void);

// Error type registration
ErrorTypeDefinition* error_type_define(const char* name, const char* description);
ErrorVariant* error_type_add_variant(ErrorTypeDefinition* type_def, 
                                     const char* variant_name,
                                     const char* description,
                                     int error_code);

// Field definition
void error_variant_add_string_field(ErrorVariant* variant, const char* name, 
                                   const char* description, bool required);
void error_variant_add_int_field(ErrorVariant* variant, const char* name,
                                const char* description, bool required,
                                int64_t min_val, int64_t max_val);
void error_variant_add_float_field(ErrorVariant* variant, const char* name,
                                  const char* description, bool required,
                                  double min_val, double max_val);
void error_variant_add_bool_field(ErrorVariant* variant, const char* name,
                                 const char* description, bool required);
void error_variant_add_enum_field(ErrorVariant* variant, const char* name,
                                 const char* description, bool required,
                                 const char** valid_values, int value_count);

// Type inheritance
void error_type_set_parent(ErrorTypeDefinition* child, ErrorTypeDefinition* parent);
bool error_type_is_subtype_of(ErrorTypeDefinition* child, ErrorTypeDefinition* parent);
ErrorTypeDefinition** error_type_get_ancestors(ErrorTypeDefinition* type, int* ancestor_count);

// Type lookup
ErrorTypeDefinition* error_type_find_by_name(const char* name);
ErrorTypeDefinition* error_type_find_by_id(uint64_t type_id);
ErrorVariant* error_variant_find_by_name(ErrorTypeDefinition* type_def, const char* variant_name);
ErrorVariant* error_variant_find_by_code(ErrorTypeDefinition* type_def, int error_code);

// =============================================================================
// Structured Error Creation and Management
// =============================================================================

// Create structured errors
StructuredError* structured_error_new(ErrorTypeDefinition* type_def, 
                                      const char* variant_name);
StructuredError* structured_error_new_with_variant(ErrorVariant* variant);
void structured_error_free(StructuredError* error);

// Field value setting
bool structured_error_set_string_field(StructuredError* error, const char* field_name, 
                                       const char* value);
bool structured_error_set_int_field(StructuredError* error, const char* field_name, 
                                    int64_t value);
bool structured_error_set_float_field(StructuredError* error, const char* field_name, 
                                      double value);
bool structured_error_set_bool_field(StructuredError* error, const char* field_name, 
                                     bool value);
bool structured_error_set_enum_field(StructuredError* error, const char* field_name,
                                     const char* enum_value);

// Field value getting
bool structured_error_get_string_field(const StructuredError* error, const char* field_name,
                                       const char** value_out);
bool structured_error_get_int_field(const StructuredError* error, const char* field_name,
                                    int64_t* value_out);
bool structured_error_get_float_field(const StructuredError* error, const char* field_name,
                                      double* value_out);
bool structured_error_get_bool_field(const StructuredError* error, const char* field_name,
                                     bool* value_out);
bool structured_error_get_enum_field(const StructuredError* error, const char* field_name,
                                     const char** value_out);

// Error validation and completion
bool structured_error_validate(StructuredError* error);
void structured_error_finalize(StructuredError* error);

// Message generation
char* structured_error_generate_message(StructuredError* error);
char* structured_error_generate_hint(StructuredError* error);
char* structured_error_generate_detailed_info(StructuredError* error);

// Error chaining
void structured_error_set_cause(StructuredError* error, StructuredError* cause);
void structured_error_set_root_cause(StructuredError* error, goo_error_t* root_cause);

// =============================================================================
// Error Pattern Matching and Dispatch
// =============================================================================

// Pattern matching types
typedef enum {
    ERROR_MATCH_TYPE,           // Match by error type
    ERROR_MATCH_VARIANT,        // Match by specific variant
    ERROR_MATCH_FIELD_VALUE,    // Match by field value
    ERROR_MATCH_FIELD_RANGE,    // Match field in range
    ERROR_MATCH_FIELD_PATTERN,  // Match field with regex
    ERROR_MATCH_INHERITANCE,    // Match by inheritance relationship
    ERROR_MATCH_SEVERITY,       // Match by severity level
    ERROR_MATCH_COMBINATION     // Combination of multiple patterns
} ErrorMatchType;

// Error match pattern
typedef struct ErrorMatchPattern {
    ErrorMatchType match_type;
    
    union {
        struct {                // For type matching
            ErrorTypeDefinition* type_def;
            bool include_subtypes;
        } type_match;
        
        struct {                // For variant matching
            ErrorVariant* variant;
        } variant_match;
        
        struct {                // For field value matching
            const char* field_name;
            ErrorFieldType field_type;
            union {
                const char* string_value;
                int64_t int_value;
                double float_value;
                bool bool_value;
            } expected_value;
        } field_match;
        
        struct {                // For field range matching
            const char* field_name;
            int64_t min_value;
            int64_t max_value;
        } range_match;
        
        struct {                // For pattern matching
            const char* field_name;
            const char* regex_pattern;
        } pattern_match;
        
        struct {                // For severity matching
            ErrorSeverity min_severity;
            ErrorSeverity max_severity;
        } severity_match;
        
        struct {                // For combination matching
            ErrorMatchPattern** sub_patterns;
            int pattern_count;
            bool match_all;     // true = AND, false = OR
        } combination_match;
    };
    
} ErrorMatchPattern;

// Pattern matching functions
ErrorMatchPattern* error_match_pattern_new_type(ErrorTypeDefinition* type_def, bool include_subtypes);
ErrorMatchPattern* error_match_pattern_new_variant(ErrorVariant* variant);
ErrorMatchPattern* error_match_pattern_new_field_value(const char* field_name, 
                                                      ErrorFieldType field_type,
                                                      const void* expected_value);
ErrorMatchPattern* error_match_pattern_new_severity(ErrorSeverity min_severity, 
                                                    ErrorSeverity max_severity);
ErrorMatchPattern* error_match_pattern_new_combination(ErrorMatchPattern** patterns,
                                                      int pattern_count, bool match_all);
void error_match_pattern_free(ErrorMatchPattern* pattern);

// Pattern matching execution
bool structured_error_matches_pattern(const StructuredError* error, 
                                      const ErrorMatchPattern* pattern);
StructuredError** structured_error_filter_by_pattern(StructuredError** errors, int error_count,
                                                     const ErrorMatchPattern* pattern,
                                                     int* matched_count);

// =============================================================================
// Error Dispatch and Handling
// =============================================================================

// Error handler function type
typedef void (*StructuredErrorHandler)(const StructuredError* error, void* context);

// Error dispatch entry
typedef struct ErrorDispatchEntry {
    ErrorMatchPattern* pattern;         // Pattern to match
    StructuredErrorHandler handler;     // Handler function
    void* handler_context;              // Context for handler
    int priority;                       // Handler priority (higher = first)
    const char* handler_name;           // Name for debugging
} ErrorDispatchEntry;

// Error dispatcher
typedef struct ErrorDispatcher {
    ErrorDispatchEntry* entries;        // Array of dispatch entries
    int entry_count;                    // Number of entries
    int entry_capacity;                 // Capacity of entries array
    
    // Default handlers
    StructuredErrorHandler default_handler;     // Fallback handler
    void* default_context;                      // Context for default handler
    
    // Statistics
    struct {
        uint64_t dispatches_performed;
        uint64_t patterns_matched;
        uint64_t default_handler_calls;
    } stats;
    
} ErrorDispatcher;

// Dispatcher management
ErrorDispatcher* error_dispatcher_new(void);
void error_dispatcher_free(ErrorDispatcher* dispatcher);

// Handler registration
void error_dispatcher_register_handler(ErrorDispatcher* dispatcher,
                                       ErrorMatchPattern* pattern,
                                       StructuredErrorHandler handler,
                                       void* context,
                                       int priority,
                                       const char* handler_name);
void error_dispatcher_set_default_handler(ErrorDispatcher* dispatcher,
                                          StructuredErrorHandler handler,
                                          void* context);

// Error dispatch
void error_dispatcher_dispatch(ErrorDispatcher* dispatcher, const StructuredError* error);
int error_dispatcher_dispatch_batch(ErrorDispatcher* dispatcher, 
                                    StructuredError** errors, int error_count);

// =============================================================================
// Integration with Existing Error System
// =============================================================================

// Convert between structured and runtime errors
goo_error_t* structured_error_to_runtime_error(const StructuredError* structured_error);
StructuredError* runtime_error_to_structured_error(const goo_error_t* runtime_error,
                                                   ErrorTypeDefinition* target_type);

// Integration with error unions
goo_error_union_t* structured_error_to_error_union(StructuredError* structured_error);
StructuredError* error_union_to_structured_error(goo_error_union_t* error_union,
                                                 ErrorTypeDefinition* target_type);

// Integration with error aggregation
void error_aggregator_add_structured_error(void* aggregator, StructuredError* error);
StructuredError** error_aggregator_get_structured_errors(void* aggregator, int* count);

// =============================================================================
// Annotation Support and Compiler Integration
// =============================================================================

// Annotation processing
typedef struct ErrorHierarchyAnnotation {
    const char* annotation_name;        // "@error_hierarchy"
    const char* type_name;              // Name of the error type
    const char* parent_type;            // Parent type (for inheritance)
    bool is_abstract;                   // Whether type is abstract
    
    // Variant definitions (parsed from source)
    struct {
        const char* variant_name;
        int error_code;
        const char* field_definitions;   // Serialized field definitions
    }* variants;
    int variant_count;
    
} ErrorHierarchyAnnotation;

// Annotation processing functions
ErrorHierarchyAnnotation* parse_error_hierarchy_annotation(const char* annotation_text);
void error_hierarchy_annotation_free(ErrorHierarchyAnnotation* annotation);
ErrorTypeDefinition* process_error_hierarchy_annotation(ErrorHierarchyAnnotation* annotation);

// Compiler hooks
void register_error_hierarchy_annotation_processor(void);
void error_hierarchy_process_source_file(const char* source_file);

// =============================================================================
// Utility Functions and Macros
// =============================================================================

// Convenience macros
#define DEFINE_ERROR_TYPE(name, desc) \
    error_type_define((name), (desc))

#define ADD_ERROR_VARIANT(type, variant_name, desc, code) \
    error_type_add_variant((type), (variant_name), (desc), (code))

#define SET_ERROR_STRING(error, field, value) \
    structured_error_set_string_field((error), (field), (value))

#define SET_ERROR_INT(error, field, value) \
    structured_error_set_int_field((error), (field), (value))

#define MATCH_ERROR_TYPE(type_def) \
    error_match_pattern_new_type((type_def), false)

#define MATCH_ERROR_VARIANT(variant) \
    error_match_pattern_new_variant((variant))

// Helper functions
const char* error_field_type_to_string(ErrorFieldType type);
const char* error_match_type_to_string(ErrorMatchType type);
void print_error_type_definition(const ErrorTypeDefinition* type_def);
void print_structured_error(const StructuredError* error);
void print_error_hierarchy_stats(const ErrorHierarchy* hierarchy);

// JSON serialization (for tooling and debugging)
char* structured_error_to_json(const StructuredError* error);
StructuredError* structured_error_from_json(const char* json_str);
char* error_type_definition_to_json(const ErrorTypeDefinition* type_def);

// =============================================================================
// Configuration and Performance
// =============================================================================

// Configuration options
typedef struct ErrorHierarchyConfig {
    bool enable_inheritance;            // Enable inheritance features
    bool enable_pattern_matching;       // Enable pattern matching
    bool enable_json_serialization;     // Enable JSON support
    bool enable_statistics;             // Collect performance statistics
    
    // Memory management
    int initial_type_capacity;          // Initial capacity for type registry
    int initial_error_capacity;         // Initial capacity for error storage
    
    // Performance tuning
    bool cache_type_lookups;           // Cache type lookup results
    bool cache_inheritance_queries;    // Cache inheritance relationships
    int max_inheritance_depth;         // Maximum inheritance depth
    
} ErrorHierarchyConfig;

// Configure the error hierarchy system
void configure_error_hierarchy_system(const ErrorHierarchyConfig* config);
ErrorHierarchyConfig* get_error_hierarchy_config(void);

// Performance monitoring
typedef struct ErrorHierarchyStats {
    uint64_t types_registered;
    uint64_t variants_defined;
    uint64_t errors_created;
    uint64_t pattern_matches_performed;
    uint64_t inheritance_queries;
    uint64_t type_lookups;
    
    // Memory usage
    uint64_t memory_used_bytes;
    uint64_t peak_memory_bytes;
    
    // Performance metrics
    double average_lookup_time_ms;
    double average_match_time_ms;
    
} ErrorHierarchyStats;

ErrorHierarchyStats get_error_hierarchy_stats(void);
void print_error_hierarchy_performance_report(void);

#endif // GOO_ERROR_HIERARCHIES_H