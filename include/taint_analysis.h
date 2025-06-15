#ifndef GOO_TAINT_ANALYSIS_H
#define GOO_TAINT_ANALYSIS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include "security_framework.h"
#include "ergonomic_errors.h"
#include "ast.h"
#include "types.h"

// Forward declarations
typedef struct TaintAnalyzer TaintAnalyzer;
typedef struct TaintInferenceEngine TaintInferenceEngine;
typedef struct TaintPropagationRule TaintPropagationRule;
typedef struct CompilerTaintIntegration CompilerTaintIntegration;

// Configuration helpers
typedef struct TaintAnalysisConfig {
    bool enable_automatic_inference;
    bool enable_interprocedural_analysis;
    bool enable_statistical_inference;
    bool enable_machine_learning;
    double minimum_confidence_threshold;
    int max_analysis_depth;
    bool generate_runtime_checks;
    bool integrate_with_security_framework;
} TaintAnalysisConfig;

// Taint source types for automatic inference
typedef enum {
    TAINT_SOURCE_NONE = 0,
    TAINT_SOURCE_USER_INPUT,        // stdin, command args, form input
    TAINT_SOURCE_NETWORK_SOCKET,    // network data
    TAINT_SOURCE_FILE_READ,         // file system reads
    TAINT_SOURCE_DATABASE_QUERY,    // database results
    TAINT_SOURCE_ENVIRONMENT_VAR,   // environment variables
    TAINT_SOURCE_EXTERNAL_API,      // external API responses
    TAINT_SOURCE_CRYPTO_WEAK,       // weak cryptographic operations
    TAINT_SOURCE_UNVALIDATED_CAST,  // unsafe type casts
    TAINT_SOURCE_UNSAFE_OPERATION,  // unsafe memory operations
    TAINT_SOURCE_DESERIALIZATION,   // deserialized data
    TAINT_SOURCE_REFLECTION,        // reflection-based operations
    TAINT_SOURCE_DYNAMIC_CODE,      // dynamically generated code
    TAINT_SOURCE_FOREIGN_FUNCTION   // foreign function interface
} TaintSourceType;

// Taint sink types (dangerous operations)
typedef enum {
    TAINT_SINK_NONE = 0,
    TAINT_SINK_COMMAND_EXECUTION,   // system(), exec(), etc.
    TAINT_SINK_SQL_QUERY,           // SQL database queries
    TAINT_SINK_FILE_WRITE,          // file system writes
    TAINT_SINK_NETWORK_SEND,        // network transmission
    TAINT_SINK_EVAL_EXPRESSION,     // dynamic code evaluation
    TAINT_SINK_FORMAT_STRING,       // printf-like format strings
    TAINT_SINK_MEMORY_ALLOCATION,   // dynamic memory allocation
    TAINT_SINK_BUFFER_WRITE,        // buffer write operations
    TAINT_SINK_POINTER_ARITHMETIC,  // pointer manipulation
    TAINT_SINK_SERIALIZATION,       // data serialization
    TAINT_SINK_LOGGING,             // logging sensitive data
    TAINT_SINK_CRYPTO_KEY,          // cryptographic key usage
    TAINT_SINK_HTML_OUTPUT,         // HTML generation (XSS risk)
    TAINT_SINK_URL_REDIRECT,        // URL redirection
    TAINT_SINK_LDAP_QUERY          // LDAP injection risk
} TaintSinkType;

// Sanitization function types
typedef enum {
    SANITIZE_NONE = 0,
    SANITIZE_HTML_ESCAPE,           // HTML entity encoding
    SANITIZE_SQL_ESCAPE,            // SQL parameter binding
    SANITIZE_SHELL_ESCAPE,          // Shell command escaping
    SANITIZE_URL_ENCODE,            // URL encoding
    SANITIZE_INPUT_VALIDATION,      // Input format validation
    SANITIZE_LENGTH_CHECK,          // Buffer length validation
    SANITIZE_REGEX_FILTER,          // Regular expression filtering
    SANITIZE_WHITELIST_CHECK,       // Whitelist validation
    SANITIZE_CRYPTO_HASH,           // Cryptographic hashing
    SANITIZE_NORMALIZE_UNICODE,     // Unicode normalization
    SANITIZE_REMOVE_NULLBYTES,      // Null byte removal
    SANITIZE_PATH_CANONICALIZE,     // Path canonicalization
    SANITIZE_JSON_ESCAPE,           // JSON escaping
    SANITIZE_XML_ESCAPE,            // XML escaping
    SANITIZE_CUSTOM                 // Custom sanitization function
} SanitizationType;

// Taint propagation information
typedef struct TaintInfo {
    TaintLevel level;
    TaintSourceType source_type;
    uint64_t source_id;
    char source_location[128];
    char source_function[64];
    
    // Propagation chain
    struct TaintInfo** parents;
    size_t parent_count;
    size_t parent_capacity;
    
    // Sanitization status
    bool is_sanitized;
    SanitizationType sanitization_type;
    char sanitization_function[64];
    
    // Validation status
    bool is_validated;
    char validation_function[64];
    char validation_pattern[128];
    
    // Confidence and analysis metadata
    double confidence_score;     // 0.0 to 1.0
    bool requires_manual_review;
    char analysis_notes[256];
    
    uint64_t creation_time;
    uint64_t last_propagation_time;
} TaintInfo;

// AST node taint annotation
typedef struct ASTTaintAnnotation {
    ASTNode* node;
    TaintInfo* taint_info;
    
    // Compiler analysis metadata
    bool is_source;
    bool is_sink;
    bool requires_sanitization;
    
    TaintSourceType source_type;
    TaintSinkType sink_type;
    SanitizationType required_sanitization;
    
    // Flow analysis
    struct ASTTaintAnnotation** data_flow_inputs;
    struct ASTTaintAnnotation** data_flow_outputs;
    size_t input_count;
    size_t output_count;
    
    struct ASTTaintAnnotation* next;
} ASTTaintAnnotation;

// Taint propagation rule
typedef struct TaintPropagationRule {
    char rule_name[64];
    char operation_pattern[128];    // Regex pattern for operation matching
    
    // Input/output taint relationships
    enum {
        PROPAGATION_PRESERVE,       // Output taint = input taint
        PROPAGATION_COMBINE,        // Output taint = max(input taints)
        PROPAGATION_ELEVATE,        // Output taint = input taint + 1 level
        PROPAGATION_SANITIZE,       // Output taint = none (if properly sanitized)
        PROPAGATION_CONDITIONAL,    // Custom condition function
        PROPAGATION_CUSTOM          // Custom propagation function
    } propagation_type;
    
    // Custom propagation logic
    TaintLevel (*custom_propagation)(TaintLevel* inputs, size_t input_count, 
                                   const char* operation, void* context);
    
    // Conditional sanitization
    bool (*sanitization_condition)(TaintLevel input_taint, const char* operation, 
                                  SanitizationType sanitization, void* context);
    
    // Rule metadata
    bool is_enabled;
    double confidence;
    char description[256];
    
    struct TaintPropagationRule* next;
} TaintPropagationRule;

// Taint inference engine for automatic source/sink detection
typedef struct TaintInferenceEngine {
    // Function signature analysis
    struct {
        char function_name[64];
        char parameter_pattern[128];  // Regex for parameter matching
        TaintSourceType source_type;
        TaintSinkType sink_type;
        SanitizationType recommended_sanitization;
        double confidence;
    } *function_signatures;
    size_t signature_count;
    size_t signature_capacity;
    
    // Variable name analysis
    struct {
        char variable_pattern[128];   // Regex for variable name matching
        TaintSourceType likely_source;
        double confidence;
    } *variable_patterns;
    size_t variable_pattern_count;
    
    // Type-based inference
    struct {
        char type_name[64];
        TaintSourceType default_source;
        bool requires_validation;
        SanitizationType recommended_sanitization;
    } *type_annotations;
    size_t type_annotation_count;
    
    // Statistical inference
    struct {
        char operation_pattern[128];
        uint64_t taint_source_count;
        uint64_t total_occurrences;
        double taint_probability;
    } *statistical_data;
    size_t statistical_count;
    
    // Machine learning model (simplified)
    struct {
        bool is_trained;
        char model_file[256];
        double feature_weights[32];
        size_t feature_count;
    } ml_model;
    
    pthread_mutex_t inference_mutex;
} TaintInferenceEngine;

// Main taint analyzer
typedef struct TaintAnalyzer {
    SecurityContext* security_context;
    TaintInferenceEngine* inference_engine;
    
    // AST analysis state
    ASTTaintAnnotation** annotations;
    size_t annotation_count;
    size_t annotation_capacity;
    
    // Propagation rules
    TaintPropagationRule* rules;
    TaintPropagationRule* rules_tail;
    size_t rule_count;
    
    // Analysis configuration
    TaintAnalysisConfig config;
    
    // Analysis results
    struct {
        uint64_t total_nodes_analyzed;
        uint64_t taint_sources_found;
        uint64_t taint_sinks_found;
        uint64_t potential_violations;
        uint64_t confirmed_violations;
        uint64_t false_positives;
        
        // Performance metrics
        uint64_t analysis_time_ns;
        uint64_t inference_time_ns;
        uint64_t propagation_time_ns;
        
        // Effectiveness metrics
        double precision;
        double recall;
        double f1_score;
    } statistics;
    
    pthread_mutex_t analyzer_mutex;
} TaintAnalyzer;

// Compiler integration for taint analysis
typedef struct CompilerTaintIntegration {
    TaintAnalyzer* analyzer;
    
    // Compiler phase integration
    bool integrate_during_parsing;
    bool integrate_during_type_checking;
    bool integrate_during_optimization;
    bool integrate_during_code_generation;
    
    // Generated code integration
    bool generate_runtime_taint_checks;
    bool generate_sanitization_calls;
    bool generate_validation_calls;
    bool generate_audit_logging;
    
    // Error reporting integration
    void (*report_taint_violation)(const char* message, const char* location, 
                                  TaintLevel taint_level, void* context);
    void (*report_missing_sanitization)(const char* function, const char* parameter,
                                       SanitizationType required, void* context);
    void* error_context;
    
    // Symbol table integration
    struct {
        char symbol_name[64];
        TaintInfo* taint_info;
        bool is_function_parameter;
        bool is_return_value;
        bool is_global_variable;
    } *symbol_annotations;
    size_t symbol_count;
    
    pthread_mutex_t integration_mutex;
} CompilerTaintIntegration;

// Core taint analysis operations
TaintAnalyzer* taint_analyzer_create(SecurityContext* security_context);
void taint_analyzer_destroy(TaintAnalyzer* analyzer);
Result_void_ptr taint_analyzer_initialize(TaintAnalyzer* analyzer);

// AST analysis
Result_void_ptr taint_analyze_ast(TaintAnalyzer* analyzer, ASTNode* root);
Result_void_ptr taint_analyze_function(TaintAnalyzer* analyzer, ASTNode* function_node);
Result_void_ptr taint_analyze_expression(TaintAnalyzer* analyzer, ASTNode* expr_node);
Result_void_ptr taint_analyze_statement(TaintAnalyzer* analyzer, ASTNode* stmt_node);

// Taint propagation
Result_void_ptr taint_propagate_through_operation(TaintAnalyzer* analyzer, 
                                                ASTNode* operation, 
                                                TaintInfo** input_taints, 
                                                size_t input_count,
                                                TaintInfo** output_taint);

// Source and sink detection
Result_bool taint_is_source(TaintAnalyzer* analyzer, ASTNode* node, TaintSourceType* source_type);
Result_bool taint_is_sink(TaintAnalyzer* analyzer, ASTNode* node, TaintSinkType* sink_type);
Result_void_ptr taint_check_sink_safety(TaintAnalyzer* analyzer, ASTNode* sink_node, TaintInfo* input_taint);

// Sanitization analysis
Result_bool taint_is_sanitized(TaintAnalyzer* analyzer, TaintInfo* taint_info, SanitizationType required_type);
Result_void_ptr taint_apply_sanitization(TaintAnalyzer* analyzer, TaintInfo* taint_info, 
                                        SanitizationType sanitization_type, const char* function_name);

// Inference engine operations
TaintInferenceEngine* taint_inference_engine_create(void);
void taint_inference_engine_destroy(TaintInferenceEngine* engine);
Result_void_ptr taint_inference_add_function_signature(TaintInferenceEngine* engine,
                                                      const char* function_name,
                                                      TaintSourceType source_type,
                                                      TaintSinkType sink_type);
Result_void_ptr taint_inference_train_on_codebase(TaintInferenceEngine* engine, ASTNode* root);
Result_void_ptr taint_inference_infer_sources_and_sinks(TaintInferenceEngine* engine, ASTNode* node);

// Propagation rule management
Result_void_ptr taint_add_propagation_rule(TaintAnalyzer* analyzer, TaintPropagationRule* rule);
Result_void_ptr taint_remove_propagation_rule(TaintAnalyzer* analyzer, const char* rule_name);
TaintPropagationRule* taint_find_propagation_rule(TaintAnalyzer* analyzer, const char* operation);

// Compiler integration
CompilerTaintIntegration* compiler_taint_integration_create(TaintAnalyzer* analyzer);
void compiler_taint_integration_destroy(CompilerTaintIntegration* integration);
Result_void_ptr compiler_taint_integrate_parsing(CompilerTaintIntegration* integration, ASTNode* node);
Result_void_ptr compiler_taint_integrate_type_checking(CompilerTaintIntegration* integration, TypeChecker* type_checker);
Result_void_ptr compiler_taint_generate_runtime_checks(CompilerTaintIntegration* integration, ASTNode* node);

// Built-in taint sources and sinks
Result_void_ptr taint_register_builtin_sources(TaintAnalyzer* analyzer);
Result_void_ptr taint_register_builtin_sinks(TaintAnalyzer* analyzer);
Result_void_ptr taint_register_builtin_sanitizers(TaintAnalyzer* analyzer);

// Annotation helpers
ASTTaintAnnotation* ast_taint_annotation_create(ASTNode* node, TaintInfo* taint_info);
void ast_taint_annotation_destroy(ASTTaintAnnotation* annotation);
ASTTaintAnnotation* ast_find_taint_annotation(TaintAnalyzer* analyzer, ASTNode* node);
Result_void_ptr ast_set_taint_annotation(TaintAnalyzer* analyzer, ASTNode* node, TaintInfo* taint_info);

// Utility functions
TaintInfo* taint_info_create(TaintLevel level, TaintSourceType source_type, const char* source_location);
void taint_info_destroy(TaintInfo* taint_info);
TaintInfo* taint_info_combine(TaintInfo* taint1, TaintInfo* taint2);
TaintInfo* taint_info_clone(TaintInfo* taint_info);

// Built-in propagation rules
TaintPropagationRule* taint_rule_create_assignment(void);
TaintPropagationRule* taint_rule_create_arithmetic(void);
TaintPropagationRule* taint_rule_create_comparison(void);
TaintPropagationRule* taint_rule_create_string_operation(void);
TaintPropagationRule* taint_rule_create_memory_operation(void);
TaintPropagationRule* taint_rule_create_function_call(void);

TaintAnalysisConfig taint_analysis_config_default(void);
TaintAnalysisConfig taint_analysis_config_strict(void);
TaintAnalysisConfig taint_analysis_config_permissive(void);
Result_void_ptr taint_analyzer_configure(TaintAnalyzer* analyzer, TaintAnalysisConfig config);

// Statistics and reporting
typedef struct TaintAnalysisReport {
    uint64_t total_functions_analyzed;
    uint64_t taint_sources_found;
    uint64_t taint_sinks_found;
    uint64_t potential_vulnerabilities;
    uint64_t confirmed_vulnerabilities;
    uint64_t sanitization_points;
    uint64_t validation_points;
    
    // Violation breakdown by type
    uint64_t command_injection_risks;
    uint64_t sql_injection_risks;
    uint64_t xss_risks;
    uint64_t buffer_overflow_risks;
    uint64_t path_traversal_risks;
    uint64_t deserialization_risks;
    
    // Analysis effectiveness
    double precision;
    double recall;
    double false_positive_rate;
    uint64_t analysis_time_ms;
    
    // Recommendations
    char* recommendations;
    size_t recommendation_count;
} TaintAnalysisReport;

TaintAnalysisReport taint_analysis_generate_report(TaintAnalyzer* analyzer);
void taint_analysis_report_destroy(TaintAnalysisReport* report);
Result_void_ptr taint_analysis_export_report(TaintAnalysisReport* report, const char* output_file);

// Error codes specific to taint analysis
#define ERROR_TAINT_ANALYSIS_FAILED      0x6001
#define ERROR_TAINT_INFERENCE_FAILED     0x6002
#define ERROR_TAINT_PROPAGATION_FAILED   0x6003
#define ERROR_SANITIZATION_MISSING      0x6004
#define ERROR_VALIDATION_REQUIRED       0x6005
#define ERROR_UNSAFE_TAINT_FLOW         0x6006
#define ERROR_CONFIDENCE_TOO_LOW        0x6007

#endif // GOO_TAINT_ANALYSIS_H