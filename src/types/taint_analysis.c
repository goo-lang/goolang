#include "../../include/taint_analysis.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <regex.h>
#include <math.h>

// Utility functions
static uint64_t get_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_unique_id(void) {
    static _Atomic uint64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// Configuration helpers
TaintAnalysisConfig taint_analysis_config_default(void) {
    return (TaintAnalysisConfig) {
        .enable_automatic_inference = true,
        .enable_interprocedural_analysis = true,
        .enable_statistical_inference = false,
        .enable_machine_learning = false,
        .minimum_confidence_threshold = 0.7,
        .max_analysis_depth = 50,
        .generate_runtime_checks = true,
        .integrate_with_security_framework = true
    };
}

TaintAnalysisConfig taint_analysis_config_strict(void) {
    TaintAnalysisConfig config = taint_analysis_config_default();
    config.minimum_confidence_threshold = 0.5;
    config.max_analysis_depth = 100;
    config.enable_statistical_inference = true;
    return config;
}

TaintAnalysisConfig taint_analysis_config_permissive(void) {
    TaintAnalysisConfig config = taint_analysis_config_default();
    config.minimum_confidence_threshold = 0.9;
    config.max_analysis_depth = 25;
    config.generate_runtime_checks = false;
    return config;
}

// TaintInfo operations
TaintInfo* taint_info_create(TaintLevel level, TaintSourceType source_type, const char* source_location) {
    TaintInfo* info = calloc(1, sizeof(TaintInfo));
    if (!info) return NULL;
    
    info->level = level;
    info->source_type = source_type;
    info->source_id = generate_unique_id();
    
    if (source_location) {
        strncpy(info->source_location, source_location, sizeof(info->source_location) - 1);
        info->source_location[sizeof(info->source_location) - 1] = '\0';
    }
    
    info->confidence_score = 1.0;
    info->creation_time = get_monotonic_time_ns();
    info->last_propagation_time = info->creation_time;
    
    return info;
}

void taint_info_destroy(TaintInfo* taint_info) {
    if (!taint_info) return;
    
    if (taint_info->parents) {
        for (size_t i = 0; i < taint_info->parent_count; i++) {
            // Don't recursively destroy parents to avoid cycles
            taint_info->parents[i] = NULL;
        }
        free(taint_info->parents);
    }
    
    free(taint_info);
}

TaintInfo* taint_info_combine(TaintInfo* taint1, TaintInfo* taint2) {
    if (!taint1 && !taint2) return NULL;
    if (!taint1) return taint_info_clone(taint2);
    if (!taint2) return taint_info_clone(taint1);
    
    // Create new combined taint info
    TaintLevel combined_level = (taint1->level > taint2->level) ? taint1->level : taint2->level;
    TaintInfo* combined = taint_info_create(combined_level, taint1->source_type, taint1->source_location);
    if (!combined) return NULL;
    
    // Set up parent tracking
    combined->parent_capacity = 2;
    combined->parents = calloc(combined->parent_capacity, sizeof(TaintInfo*));
    if (combined->parents) {
        combined->parents[0] = taint1;
        combined->parents[1] = taint2;
        combined->parent_count = 2;
    }
    
    // Combine sanitization status (both must be sanitized)
    combined->is_sanitized = taint1->is_sanitized && taint2->is_sanitized;
    combined->is_validated = taint1->is_validated && taint2->is_validated;
    
    // Use minimum confidence
    combined->confidence_score = (taint1->confidence_score < taint2->confidence_score) ? 
                                taint1->confidence_score : taint2->confidence_score;
    
    snprintf(combined->analysis_notes, sizeof(combined->analysis_notes), 
             "Combined from sources: %s, %s", taint1->source_location, taint2->source_location);
    
    return combined;
}

TaintInfo* taint_info_clone(TaintInfo* taint_info) {
    if (!taint_info) return NULL;
    
    TaintInfo* clone = calloc(1, sizeof(TaintInfo));
    if (!clone) return NULL;
    
    memcpy(clone, taint_info, sizeof(TaintInfo));
    
    // Deep copy parent array
    if (taint_info->parents && taint_info->parent_count > 0) {
        clone->parents = calloc(taint_info->parent_capacity, sizeof(TaintInfo*));
        if (clone->parents) {
            memcpy(clone->parents, taint_info->parents, 
                   taint_info->parent_count * sizeof(TaintInfo*));
        }
    }
    
    return clone;
}

// AST annotation operations
ASTTaintAnnotation* ast_taint_annotation_create(ASTNode* node, TaintInfo* taint_info) {
    ASTTaintAnnotation* annotation = calloc(1, sizeof(ASTTaintAnnotation));
    if (!annotation) return NULL;
    
    annotation->node = node;
    annotation->taint_info = taint_info;
    annotation->is_source = false;
    annotation->is_sink = false;
    annotation->requires_sanitization = false;
    
    return annotation;
}

void ast_taint_annotation_destroy(ASTTaintAnnotation* annotation) {
    if (!annotation) return;
    
    if (annotation->taint_info) {
        taint_info_destroy(annotation->taint_info);
    }
    
    if (annotation->data_flow_inputs) {
        free(annotation->data_flow_inputs);
    }
    
    if (annotation->data_flow_outputs) {
        free(annotation->data_flow_outputs);
    }
    
    free(annotation);
}

// Taint inference engine operations
TaintInferenceEngine* taint_inference_engine_create(void) {
    TaintInferenceEngine* engine = calloc(1, sizeof(TaintInferenceEngine));
    if (!engine) return NULL;
    
    // Initialize function signatures with common patterns
    engine->signature_capacity = 100;
    engine->function_signatures = calloc(engine->signature_capacity, 
                                        sizeof(*engine->function_signatures));
    if (!engine->function_signatures) {
        free(engine);
        return NULL;
    }
    
    // Initialize variable patterns
    engine->variable_pattern_count = 0;
    engine->variable_patterns = calloc(50, sizeof(*engine->variable_patterns));
    
    // Initialize type annotations
    engine->type_annotation_count = 0;
    engine->type_annotations = calloc(50, sizeof(*engine->type_annotations));
    
    pthread_mutex_init(&engine->inference_mutex, NULL);
    
    return engine;
}

void taint_inference_engine_destroy(TaintInferenceEngine* engine) {
    if (!engine) return;
    
    pthread_mutex_destroy(&engine->inference_mutex);
    
    free(engine->function_signatures);
    free(engine->variable_patterns);
    free(engine->type_annotations);
    free(engine->statistical_data);
    
    free(engine);
}

// Main taint analyzer operations
TaintAnalyzer* taint_analyzer_create(SecurityContext* security_context) {
    TaintAnalyzer* analyzer = calloc(1, sizeof(TaintAnalyzer));
    if (!analyzer) return NULL;
    
    analyzer->security_context = security_context;
    analyzer->inference_engine = taint_inference_engine_create();
    if (!analyzer->inference_engine) {
        free(analyzer);
        return NULL;
    }
    
    // Initialize annotation storage
    analyzer->annotation_capacity = 1000;
    analyzer->annotations = calloc(analyzer->annotation_capacity, sizeof(ASTTaintAnnotation*));
    if (!analyzer->annotations) {
        taint_inference_engine_destroy(analyzer->inference_engine);
        free(analyzer);
        return NULL;
    }
    
    // Set default configuration
    analyzer->config = taint_analysis_config_default();
    
    pthread_mutex_init(&analyzer->analyzer_mutex, NULL);
    
    return analyzer;
}

void taint_analyzer_destroy(TaintAnalyzer* analyzer) {
    if (!analyzer) return;
    
    pthread_mutex_destroy(&analyzer->analyzer_mutex);
    
    // Clean up annotations
    if (analyzer->annotations) {
        for (size_t i = 0; i < analyzer->annotation_count; i++) {
            ast_taint_annotation_destroy(analyzer->annotations[i]);
        }
        free(analyzer->annotations);
    }
    
    // Clean up propagation rules
    TaintPropagationRule* rule = analyzer->rules;
    while (rule) {
        TaintPropagationRule* next = rule->next;
        free(rule);
        rule = next;
    }
    
    taint_inference_engine_destroy(analyzer->inference_engine);
    free(analyzer);
}

Result_void_ptr taint_analyzer_initialize(TaintAnalyzer* analyzer) {
    if (!analyzer) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid analyzer pointer";
        }
        return ERR_PTR(err);
    }
    
    uint64_t start_time = get_monotonic_time_ns();
    
    // Register built-in sources, sinks, and sanitizers
    Result_void_ptr result = taint_register_builtin_sources(analyzer);
    if (result.is_error) return result;
    
    result = taint_register_builtin_sinks(analyzer);
    if (result.is_error) return result;
    
    result = taint_register_builtin_sanitizers(analyzer);
    if (result.is_error) return result;
    
    analyzer->statistics.analysis_time_ns = get_monotonic_time_ns() - start_time;
    
    return OK_PTR(analyzer);
}

// Built-in taint sources registration
Result_void_ptr taint_register_builtin_sources(TaintAnalyzer* analyzer) {
    if (!analyzer || !analyzer->inference_engine) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid analyzer or inference engine";
        }
        return ERR_PTR(err);
    }
    
    TaintInferenceEngine* engine = analyzer->inference_engine;
    
    // Register common taint sources
    struct {
        const char* function_name;
        TaintSourceType source_type;
        double confidence;
    } builtin_sources[] = {
        {"scanf", TAINT_SOURCE_USER_INPUT, 0.95},
        {"fgets", TAINT_SOURCE_USER_INPUT, 0.90},
        {"getline", TAINT_SOURCE_USER_INPUT, 0.90},
        {"read", TAINT_SOURCE_FILE_READ, 0.85},
        {"fread", TAINT_SOURCE_FILE_READ, 0.85},
        {"recv", TAINT_SOURCE_NETWORK_SOCKET, 0.90},
        {"recvfrom", TAINT_SOURCE_NETWORK_SOCKET, 0.90},
        {"getenv", TAINT_SOURCE_ENVIRONMENT_VAR, 0.95},
        {"argv", TAINT_SOURCE_USER_INPUT, 0.95},
        {"stdin", TAINT_SOURCE_USER_INPUT, 0.95},
        {"socket_read", TAINT_SOURCE_NETWORK_SOCKET, 0.90},
        {"db_query", TAINT_SOURCE_DATABASE_QUERY, 0.85},
        {"http_request", TAINT_SOURCE_EXTERNAL_API, 0.85},
        {"file_get_contents", TAINT_SOURCE_FILE_READ, 0.80},
        {"curl_exec", TAINT_SOURCE_EXTERNAL_API, 0.85}
    };
    
    size_t builtin_count = sizeof(builtin_sources) / sizeof(builtin_sources[0]);
    
    for (size_t i = 0; i < builtin_count && engine->signature_count < engine->signature_capacity; i++) {
        strncpy(engine->function_signatures[engine->signature_count].function_name,
                builtin_sources[i].function_name, 63);
        engine->function_signatures[engine->signature_count].source_type = builtin_sources[i].source_type;
        engine->function_signatures[engine->signature_count].sink_type = TAINT_SINK_NONE;
        engine->function_signatures[engine->signature_count].confidence = builtin_sources[i].confidence;
        engine->signature_count++;
    }
    
    return OK_PTR(analyzer);
}

// Built-in taint sinks registration
Result_void_ptr taint_register_builtin_sinks(TaintAnalyzer* analyzer) {
    if (!analyzer || !analyzer->inference_engine) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid analyzer or inference engine";
        }
        return ERR_PTR(err);
    }
    
    TaintInferenceEngine* engine = analyzer->inference_engine;
    
    // Register common taint sinks
    struct {
        const char* function_name;
        TaintSinkType sink_type;
        double confidence;
    } builtin_sinks[] = {
        {"system", TAINT_SINK_COMMAND_EXECUTION, 0.95},
        {"exec", TAINT_SINK_COMMAND_EXECUTION, 0.95},
        {"popen", TAINT_SINK_COMMAND_EXECUTION, 0.90},
        {"printf", TAINT_SINK_FORMAT_STRING, 0.80},
        {"sprintf", TAINT_SINK_BUFFER_WRITE, 0.85},
        {"strcpy", TAINT_SINK_BUFFER_WRITE, 0.85},
        {"strcat", TAINT_SINK_BUFFER_WRITE, 0.85},
        {"write", TAINT_SINK_FILE_WRITE, 0.80},
        {"fwrite", TAINT_SINK_FILE_WRITE, 0.80},
        {"send", TAINT_SINK_NETWORK_SEND, 0.85},
        {"sendto", TAINT_SINK_NETWORK_SEND, 0.85},
        {"sql_query", TAINT_SINK_SQL_QUERY, 0.90},
        {"eval", TAINT_SINK_EVAL_EXPRESSION, 0.95},
        {"malloc", TAINT_SINK_MEMORY_ALLOCATION, 0.70},
        {"memcpy", TAINT_SINK_BUFFER_WRITE, 0.80}
    };
    
    size_t builtin_count = sizeof(builtin_sinks) / sizeof(builtin_sinks[0]);
    
    for (size_t i = 0; i < builtin_count && engine->signature_count < engine->signature_capacity; i++) {
        // Find existing entry or create new one
        size_t index = engine->signature_count;
        
        // Look for existing function signature
        for (size_t j = 0; j < engine->signature_count; j++) {
            if (strcmp(engine->function_signatures[j].function_name, builtin_sinks[i].function_name) == 0) {
                index = j;
                break;
            }
        }
        
        if (index == engine->signature_count) {
            // New function signature
            strncpy(engine->function_signatures[index].function_name,
                    builtin_sinks[i].function_name, 63);
            engine->function_signatures[index].source_type = TAINT_SOURCE_NONE;
            engine->signature_count++;
        }
        
        engine->function_signatures[index].sink_type = builtin_sinks[i].sink_type;
        engine->function_signatures[index].confidence = builtin_sinks[i].confidence;
    }
    
    return OK_PTR(analyzer);
}

// Built-in sanitization functions registration
Result_void_ptr taint_register_builtin_sanitizers(TaintAnalyzer* analyzer) {
    if (!analyzer || !analyzer->inference_engine) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid analyzer or inference engine";
        }
        return ERR_PTR(err);
    }
    
    TaintInferenceEngine* engine = analyzer->inference_engine;
    
    // Register common sanitization functions
    struct {
        const char* function_name;
        SanitizationType sanitization_type;
        double confidence;
    } builtin_sanitizers[] = {
        {"html_escape", SANITIZE_HTML_ESCAPE, 0.95},
        {"sql_escape", SANITIZE_SQL_ESCAPE, 0.95},
        {"shell_escape", SANITIZE_SHELL_ESCAPE, 0.90},
        {"url_encode", SANITIZE_URL_ENCODE, 0.90},
        {"validate_input", SANITIZE_INPUT_VALIDATION, 0.85},
        {"check_length", SANITIZE_LENGTH_CHECK, 0.80},
        {"filter_regex", SANITIZE_REGEX_FILTER, 0.85},
        {"whitelist_check", SANITIZE_WHITELIST_CHECK, 0.90},
        {"hash_data", SANITIZE_CRYPTO_HASH, 0.95},
        {"normalize_unicode", SANITIZE_NORMALIZE_UNICODE, 0.80},
        {"remove_nullbytes", SANITIZE_REMOVE_NULLBYTES, 0.85},
        {"canonicalize_path", SANITIZE_PATH_CANONICALIZE, 0.85},
        {"json_escape", SANITIZE_JSON_ESCAPE, 0.90},
        {"xml_escape", SANITIZE_XML_ESCAPE, 0.90}
    };
    
    size_t builtin_count = sizeof(builtin_sanitizers) / sizeof(builtin_sanitizers[0]);
    
    for (size_t i = 0; i < builtin_count && engine->signature_count < engine->signature_capacity; i++) {
        // Find existing entry or create new one
        size_t index = engine->signature_count;
        
        // Look for existing function signature
        for (size_t j = 0; j < engine->signature_count; j++) {
            if (strcmp(engine->function_signatures[j].function_name, builtin_sanitizers[i].function_name) == 0) {
                index = j;
                break;
            }
        }
        
        if (index == engine->signature_count) {
            // New function signature
            strncpy(engine->function_signatures[index].function_name,
                    builtin_sanitizers[i].function_name, 63);
            engine->function_signatures[index].source_type = TAINT_SOURCE_NONE;
            engine->function_signatures[index].sink_type = TAINT_SINK_NONE;
            engine->signature_count++;
        }
        
        engine->function_signatures[index].recommended_sanitization = builtin_sanitizers[i].sanitization_type;
        engine->function_signatures[index].confidence = builtin_sanitizers[i].confidence;
    }
    
    return OK_PTR(analyzer);
}

// Built-in propagation rules
TaintPropagationRule* taint_rule_create_assignment(void) {
    TaintPropagationRule* rule = calloc(1, sizeof(TaintPropagationRule));
    if (!rule) return NULL;
    
    strncpy(rule->rule_name, "assignment", sizeof(rule->rule_name) - 1);
    strncpy(rule->operation_pattern, "^(=|:=|assign)$", sizeof(rule->operation_pattern) - 1);
    rule->propagation_type = PROPAGATION_PRESERVE;
    rule->is_enabled = true;
    rule->confidence = 0.95;
    strncpy(rule->description, "Direct assignment preserves taint", sizeof(rule->description) - 1);
    
    return rule;
}

TaintPropagationRule* taint_rule_create_arithmetic(void) {
    TaintPropagationRule* rule = calloc(1, sizeof(TaintPropagationRule));
    if (!rule) return NULL;
    
    strncpy(rule->rule_name, "arithmetic", sizeof(rule->rule_name) - 1);
    strncpy(rule->operation_pattern, "^(\\+|\\-|\\*|\\/|%|<<|>>|&|\\||\\^)$", sizeof(rule->operation_pattern) - 1);
    rule->propagation_type = PROPAGATION_COMBINE;
    rule->is_enabled = true;
    rule->confidence = 0.85;
    strncpy(rule->description, "Arithmetic operations combine taint from operands", sizeof(rule->description) - 1);
    
    return rule;
}

TaintPropagationRule* taint_rule_create_string_operation(void) {
    TaintPropagationRule* rule = calloc(1, sizeof(TaintPropagationRule));
    if (!rule) return NULL;
    
    strncpy(rule->rule_name, "string_operation", sizeof(rule->rule_name) - 1);
    strncpy(rule->operation_pattern, "^(strcat|strncat|sprintf|snprintf|concat)$", sizeof(rule->operation_pattern) - 1);
    rule->propagation_type = PROPAGATION_COMBINE;
    rule->is_enabled = true;
    rule->confidence = 0.90;
    strncpy(rule->description, "String operations combine taint from all inputs", sizeof(rule->description) - 1);
    
    return rule;
}

// Compiler integration operations
CompilerTaintIntegration* compiler_taint_integration_create(TaintAnalyzer* analyzer) {
    CompilerTaintIntegration* integration = calloc(1, sizeof(CompilerTaintIntegration));
    if (!integration) return NULL;
    
    integration->analyzer = analyzer;
    
    // Set default integration options
    integration->integrate_during_parsing = false;
    integration->integrate_during_type_checking = true;
    integration->integrate_during_optimization = false;
    integration->integrate_during_code_generation = true;
    
    integration->generate_runtime_taint_checks = analyzer->config.generate_runtime_checks;
    integration->generate_sanitization_calls = true;
    integration->generate_validation_calls = true;
    integration->generate_audit_logging = true;
    
    pthread_mutex_init(&integration->integration_mutex, NULL);
    
    return integration;
}

void compiler_taint_integration_destroy(CompilerTaintIntegration* integration) {
    if (!integration) return;
    
    pthread_mutex_destroy(&integration->integration_mutex);
    
    if (integration->symbol_annotations) {
        free(integration->symbol_annotations);
    }
    
    free(integration);
}

// Configuration function
Result_void_ptr taint_analyzer_configure(TaintAnalyzer* analyzer, TaintAnalysisConfig config) {
    if (!analyzer) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid analyzer pointer";
        }
        return ERR_PTR(err);
    }
    
    pthread_mutex_lock(&analyzer->analyzer_mutex);
    analyzer->config = config;
    pthread_mutex_unlock(&analyzer->analyzer_mutex);
    
    return OK_PTR(analyzer);
}

// Stub implementations for complex analysis functions that would require full AST traversal
Result_void_ptr taint_analyze_ast(TaintAnalyzer* analyzer, ASTNode* root) {
    if (!analyzer || !root) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid analyzer or AST node";
        }
        return ERR_PTR(err);
    }
    
    // This would perform full AST traversal and taint analysis
    // For now, return success to indicate the framework is set up
    analyzer->statistics.total_nodes_analyzed++;
    
    return OK_PTR(analyzer);
}

Result_bool taint_is_source(TaintAnalyzer* analyzer, ASTNode* node, TaintSourceType* source_type) {
    if (!analyzer || !node || !source_type) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid parameters";
        }
        return ERR(bool, err);
    }
    
    // Stub implementation - would analyze AST node to determine if it's a taint source
    *source_type = TAINT_SOURCE_NONE;
    return OK(bool, false);
}

Result_bool taint_is_sink(TaintAnalyzer* analyzer, ASTNode* node, TaintSinkType* sink_type) {
    if (!analyzer || !node || !sink_type) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid parameters";
        }
        return ERR(bool, err);
    }
    
    // Stub implementation - would analyze AST node to determine if it's a taint sink
    *sink_type = TAINT_SINK_NONE;
    return OK(bool, false);
}