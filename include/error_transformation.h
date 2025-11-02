#ifndef GOO_ERROR_TRANSFORMATION_H
#define GOO_ERROR_TRANSFORMATION_H

#include "error_hierarchies.h"
#include "errors/error.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Error Transformation and Internationalization System
// =============================================================================

// Forward declarations
typedef struct ErrorTransformer ErrorTransformer;
typedef struct ErrorMessageTemplate ErrorMessageTemplate;
typedef struct ErrorLocalizationContext ErrorLocalizationContext;
typedef struct ErrorCodeRegistry ErrorCodeRegistry;
typedef struct ErrorConversionRule ErrorConversionRule;

// =============================================================================
// Error Code System
// =============================================================================

// Error code classification
typedef enum {
    ERROR_CODE_CLASS_SYSTEM        = 1000,  // System-level errors (1000-1999)
    ERROR_CODE_CLASS_CONFIG        = 2000,  // Configuration errors (2000-2999)
    ERROR_CODE_CLASS_NETWORK       = 3000,  // Network errors (3000-3999)
    ERROR_CODE_CLASS_DATABASE      = 4000,  // Database errors (4000-4999)
    ERROR_CODE_CLASS_AUTH          = 5000,  // Authentication errors (5000-5999)
    ERROR_CODE_CLASS_BUSINESS      = 6000,  // Business logic errors (6000-6999)
    ERROR_CODE_CLASS_VALIDATION    = 7000,  // Validation errors (7000-7999)
    ERROR_CODE_CLASS_IO            = 8000,  // I/O errors (8000-8999)
    ERROR_CODE_CLASS_EXTERNAL      = 9000,  // External service errors (9000-9999)
    ERROR_CODE_CLASS_CUSTOM        = 10000  // Custom application errors (10000+)
} ErrorCodeClass;

// Machine-readable error code with metadata
typedef struct MachineErrorCode {
    int code;                           // Unique error code
    ErrorCodeClass class;               // Error classification
    const char* identifier;             // Machine-readable identifier (e.g., "CONFIG_FILE_NOT_FOUND")
    const char* category;               // Category (e.g., "filesystem", "network")
    ErrorSeverity severity;             // Default severity
    
    // Metadata
    const char* component;              // Which component generated this error
    const char* operation;              // What operation was being performed
    bool is_recoverable;                // Whether error is typically recoverable
    bool is_user_error;                 // Whether this is due to user input
    
    // Associated structured error information
    ErrorTypeDefinition* structured_type;  // Associated structured error type
    const char* variant_name;               // Specific variant if applicable
    
} MachineErrorCode;

// Error code registry
typedef struct ErrorCodeRegistry {
    MachineErrorCode* codes;            // Array of registered error codes
    int code_count;                     // Number of registered codes
    int code_capacity;                  // Capacity of codes array
    
    // Lookup tables for fast access
    struct {
        int* codes;                     // Array of error codes
        MachineErrorCode** entries;     // Corresponding registry entries
        int lookup_count;               // Number of entries
    } code_lookup;
    
    struct {
        char** identifiers;             // Array of identifiers
        MachineErrorCode** entries;     // Corresponding registry entries
        int lookup_count;               // Number of entries
    } identifier_lookup;
    
    // Statistics
    struct {
        uint64_t codes_registered;
        uint64_t lookups_performed;
        uint64_t transformations_applied;
    } stats;
    
} ErrorCodeRegistry;

// =============================================================================
// Message Templates and Localization
// =============================================================================

// Supported languages for internationalization
typedef enum {
    ERROR_LANG_EN,      // English (default)
    ERROR_LANG_ES,      // Spanish
    ERROR_LANG_FR,      // French
    ERROR_LANG_DE,      // German
    ERROR_LANG_IT,      // Italian
    ERROR_LANG_PT,      // Portuguese
    ERROR_LANG_RU,      // Russian
    ERROR_LANG_ZH,      // Chinese
    ERROR_LANG_JA,      // Japanese
    ERROR_LANG_KO,      // Korean
    ERROR_LANG_CUSTOM   // Custom language support
} ErrorLanguage;

// Message template for different contexts
typedef struct ErrorMessageTemplate {
    const char* template_id;            // Unique template identifier
    const char* error_identifier;      // Associated error code identifier
    
    // Templates for different contexts
    const char* short_message;          // Brief error message
    const char* detailed_message;       // Detailed explanation
    const char* user_message;           // User-friendly message
    const char* technical_message;      // Technical/debug message
    const char* recovery_suggestion;    // How to recover from this error
    
    // Formatting information
    const char** placeholder_names;     // Names of placeholders in templates
    int placeholder_count;              // Number of placeholders
    
    // Context-specific variations
    struct {
        const char* cli_message;        // Command-line interface context
        const char* web_message;        // Web interface context
        const char* api_message;        // API response context
        const char* log_message;        // Logging context
    } context_variations;
    
} ErrorMessageTemplate;

// Localization context for error messages
typedef struct ErrorLocalizationContext {
    ErrorLanguage language;             // Target language
    const char* locale;                 // Full locale string (e.g., "en_US", "es_MX")
    const char* timezone;               // Timezone for timestamps
    
    // Formatting preferences
    const char* date_format;            // Date format preference
    const char* time_format;            // Time format preference
    const char* number_format;          // Number format preference
    
    // Cultural adaptations
    bool use_formal_language;           // Use formal vs informal language
    bool include_cultural_context;      // Include culture-specific explanations
    
    // Template cache
    struct {
        ErrorMessageTemplate** templates;  // Cached templates for this language
        int template_count;                // Number of cached templates
    } template_cache;
    
} ErrorLocalizationContext;

// =============================================================================
// Error Type Conversion and Transformation
// =============================================================================

// Conversion compatibility levels
typedef enum {
    ERROR_CONVERSION_EXACT,         // Exact type match
    ERROR_CONVERSION_COMPATIBLE,    // Compatible types (same hierarchy)
    ERROR_CONVERSION_LOSSY,         // Some information may be lost
    ERROR_CONVERSION_APPROXIMATE,   // Best-effort conversion
    ERROR_CONVERSION_IMPOSSIBLE     // Cannot convert
} ErrorConversionCompatibility;

// Error conversion rule
typedef struct ErrorConversionRule {
    // Source and target types
    ErrorTypeDefinition* source_type;      // Source error type
    const char* source_variant;            // Source variant (NULL for any)
    ErrorTypeDefinition* target_type;      // Target error type
    const char* target_variant;            // Target variant
    
    // Conversion metadata
    ErrorConversionCompatibility compatibility;  // Conversion quality
    double conversion_cost;                      // Relative cost (0.0 = free, 1.0 = expensive)
    
    // Field mapping
    struct {
        const char* source_field;       // Source field name
        const char* target_field;       // Target field name
        bool required_mapping;          // Whether this mapping is required
    }* field_mappings;
    int field_mapping_count;
    
    // Custom conversion function
    StructuredError* (*converter)(const StructuredError* source, void* context);
    void* converter_context;
    
    // Metadata
    const char* rule_name;              // Human-readable rule name
    const char* description;            // Rule description
    uint64_t usage_count;               // How often this rule has been used
    
} ErrorConversionRule;

// Error transformer - handles conversion and message generation
typedef struct ErrorTransformer {
    // Conversion rules
    ErrorConversionRule* rules;         // Array of conversion rules
    int rule_count;                     // Number of rules
    int rule_capacity;                  // Capacity of rules array
    
    // Localization support
    ErrorLocalizationContext* contexts; // Array of localization contexts
    int context_count;                  // Number of contexts
    ErrorLanguage default_language;     // Default language
    
    // Message templates
    ErrorMessageTemplate* templates;    // Array of message templates
    int template_count;                 // Number of templates
    int template_capacity;              // Capacity of templates array
    
    // Configuration
    struct {
        bool auto_convert_compatible;   // Automatically convert compatible types
        bool preserve_original_error;   // Keep reference to original error
        bool generate_stack_trace;      // Generate transformation stack trace
        int max_conversion_depth;       // Maximum conversion chain depth
    } config;
    
    // Statistics
    struct {
        uint64_t conversions_performed;
        uint64_t messages_generated;
        uint64_t cache_hits;
        uint64_t cache_misses;
    } stats;
    
} ErrorTransformer;

// =============================================================================
// Core API Functions
// =============================================================================

// System initialization
void error_transformation_system_init(void);
void error_transformation_system_shutdown(void);

// Get global transformer
ErrorTransformer* get_global_error_transformer(void);

// Error code registry management
ErrorCodeRegistry* error_code_registry_new(void);
void error_code_registry_free(ErrorCodeRegistry* registry);

// Register error codes
MachineErrorCode* error_code_register(ErrorCodeRegistry* registry,
                                     int code,
                                     ErrorCodeClass class,
                                     const char* identifier,
                                     const char* category,
                                     ErrorSeverity severity);

// Error code lookup
MachineErrorCode* error_code_find_by_code(const ErrorCodeRegistry* registry, int code);
MachineErrorCode* error_code_find_by_identifier(const ErrorCodeRegistry* registry, 
                                               const char* identifier);

// Associate error codes with structured errors
void error_code_associate_structured_type(MachineErrorCode* error_code,
                                         ErrorTypeDefinition* type_def,
                                         const char* variant_name);

// =============================================================================
// Error Transformation Functions
// =============================================================================

// Error transformer management
ErrorTransformer* error_transformer_new(void);
void error_transformer_free(ErrorTransformer* transformer);

// Conversion rule management
void error_transformer_add_conversion_rule(ErrorTransformer* transformer,
                                          ErrorConversionRule* rule);
ErrorConversionRule* error_transformer_find_conversion_rule(const ErrorTransformer* transformer,
                                                          const StructuredError* source_error,
                                                          ErrorTypeDefinition* target_type);

// Error conversion
StructuredError* error_transformer_convert(ErrorTransformer* transformer,
                                         const StructuredError* source_error,
                                         ErrorTypeDefinition* target_type);
bool error_transformer_can_convert(const ErrorTransformer* transformer,
                                  const StructuredError* source_error,
                                  ErrorTypeDefinition* target_type);

// Automatic conversion based on context
StructuredError* error_transformer_auto_convert(ErrorTransformer* transformer,
                                               const StructuredError* source_error,
                                               const char* target_context);

// =============================================================================
// Message Generation and Localization
// =============================================================================

// Localization context management
ErrorLocalizationContext* error_localization_context_new(ErrorLanguage language,
                                                         const char* locale);
void error_localization_context_free(ErrorLocalizationContext* context);

// Message template management
void error_transformer_add_message_template(ErrorTransformer* transformer,
                                           ErrorMessageTemplate* template);
ErrorMessageTemplate* error_transformer_find_template(const ErrorTransformer* transformer,
                                                     const char* error_identifier,
                                                     ErrorLanguage language);

// Message generation
char* error_transformer_generate_message(ErrorTransformer* transformer,
                                        const StructuredError* error,
                                        const ErrorLocalizationContext* context,
                                        const char* message_type);

char* error_transformer_generate_localized_message(ErrorTransformer* transformer,
                                                  const StructuredError* error,
                                                  ErrorLanguage language,
                                                  const char* message_type);

// Context-aware message generation
char* error_transformer_generate_contextual_message(ErrorTransformer* transformer,
                                                   const StructuredError* error,
                                                   const char* context_type,
                                                   const ErrorLocalizationContext* locale_context);

// =============================================================================
// Built-in Conversion Rules and Templates
// =============================================================================

// Register standard conversion rules
void error_transformer_register_standard_conversions(ErrorTransformer* transformer);

// Register standard message templates
void error_transformer_register_standard_templates(ErrorTransformer* transformer);

// Built-in conversions for common error types
ErrorConversionRule* create_system_to_user_conversion_rule(void);
ErrorConversionRule* create_technical_to_simple_conversion_rule(void);
ErrorConversionRule* create_generic_to_specific_conversion_rule(void);

// Built-in message templates for common scenarios
ErrorMessageTemplate* create_file_not_found_template(void);
ErrorMessageTemplate* create_network_timeout_template(void);
ErrorMessageTemplate* create_permission_denied_template(void);
ErrorMessageTemplate* create_validation_failed_template(void);

// =============================================================================
// Integration with Existing Error Systems
// =============================================================================

// Convert runtime errors to structured errors with transformation
StructuredError* error_transformer_convert_runtime_error(ErrorTransformer* transformer,
                                                        const goo_error_t* runtime_error,
                                                        ErrorTypeDefinition* target_type);

// Convert structured errors to runtime errors with message generation
goo_error_t* error_transformer_to_runtime_error(ErrorTransformer* transformer,
                                               const StructuredError* structured_error,
                                               const ErrorLocalizationContext* context);

// Integration with error aggregation
void error_transformer_transform_aggregated_errors(ErrorTransformer* transformer,
                                                  void* aggregator,
                                                  ErrorTypeDefinition* target_type);

// =============================================================================
// Advanced Features
// =============================================================================

// Error transformation chains
typedef struct ErrorTransformationChain {
    StructuredError** errors;           // Chain of transformed errors
    int chain_length;                   // Length of the chain
    ErrorConversionRule** rules_used;   // Rules used in transformation
    double total_cost;                  // Total transformation cost
} ErrorTransformationChain;

ErrorTransformationChain* error_transformer_create_chain(ErrorTransformer* transformer,
                                                        const StructuredError* source_error,
                                                        ErrorTypeDefinition* target_type);
void error_transformation_chain_free(ErrorTransformationChain* chain);

// Smart error classification
ErrorTypeDefinition* error_transformer_classify_error(ErrorTransformer* transformer,
                                                     const char* error_message,
                                                     const char* context);

// Error similarity analysis
double error_transformer_calculate_similarity(const StructuredError* error1,
                                             const StructuredError* error2);

// Batch transformation operations
int error_transformer_batch_convert(ErrorTransformer* transformer,
                                   StructuredError** source_errors,
                                   int error_count,
                                   ErrorTypeDefinition* target_type,
                                   StructuredError*** converted_errors);

// =============================================================================
// Utility Functions and Macros
// =============================================================================

// Convenience macros
#define REGISTER_ERROR_CODE(registry, code, class, id, category, severity) \
    error_code_register((registry), (code), (class), (id), (category), (severity))

#define CONVERT_ERROR(transformer, error, target_type) \
    error_transformer_convert((transformer), (error), (target_type))

#define LOCALIZE_ERROR(transformer, error, language) \
    error_transformer_generate_localized_message((transformer), (error), (language), "user")

// Language and locale utilities
const char* error_language_to_string(ErrorLanguage language);
ErrorLanguage error_language_from_string(const char* language_str);
const char* error_language_to_locale(ErrorLanguage language);

// Formatting helpers
char* error_format_timestamp(uint64_t timestamp_ms, const ErrorLocalizationContext* context);
char* error_format_number(double number, const ErrorLocalizationContext* context);
char* error_format_currency(double amount, const char* currency, 
                           const ErrorLocalizationContext* context);

// Template placeholder replacement
char* error_replace_placeholders(const char* template_str,
                                const StructuredError* error,
                                const ErrorLocalizationContext* context);

// Debug and introspection
void print_error_transformation_stats(const ErrorTransformer* transformer);
void print_error_code_registry(const ErrorCodeRegistry* registry);
void print_conversion_rule(const ErrorConversionRule* rule);
void print_message_template(const ErrorMessageTemplate* template);

// JSON serialization support
char* error_transformer_to_json(const ErrorTransformer* transformer);
char* error_code_registry_to_json(const ErrorCodeRegistry* registry);
char* machine_error_code_to_json(const MachineErrorCode* error_code);

// =============================================================================
// Configuration and Performance
// =============================================================================

// Configuration options
typedef struct ErrorTransformationConfig {
    bool enable_auto_conversion;           // Enable automatic error conversion
    bool enable_message_caching;           // Cache generated messages
    bool enable_template_caching;          // Cache message templates
    bool enable_similarity_analysis;       // Enable error similarity features
    
    // Performance tuning
    int max_conversion_chain_length;       // Maximum transformation chain length
    int message_cache_size;                // Size of message cache
    int template_cache_size;               // Size of template cache
    
    // Localization settings
    ErrorLanguage default_language;        // Default language for messages
    bool enable_fallback_language;         // Fall back to default if translation missing
    bool enable_cultural_adaptations;      // Enable culture-specific features
    
} ErrorTransformationConfig;

// Configure the transformation system
void configure_error_transformation_system(const ErrorTransformationConfig* config);
ErrorTransformationConfig* get_error_transformation_config(void);

// Performance monitoring
typedef struct ErrorTransformationStats {
    uint64_t transformations_performed;
    uint64_t messages_generated;
    uint64_t template_cache_hits;
    uint64_t template_cache_misses;
    uint64_t conversion_cache_hits;
    uint64_t conversion_cache_misses;
    
    // Performance metrics
    double average_transformation_time_ms;
    double average_message_generation_time_ms;
    
    // Memory usage
    uint64_t memory_used_bytes;
    uint64_t peak_memory_bytes;
    
} ErrorTransformationStats;

ErrorTransformationStats get_error_transformation_stats(void);
void print_error_transformation_performance_report(void);

#endif // GOO_ERROR_TRANSFORMATION_H