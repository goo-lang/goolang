#define _POSIX_C_SOURCE 200809L
#include "error_transformation.h"
#include "error_hierarchies.h"
#include "error_aggregation.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <regex.h>
#include <math.h>
#include <ctype.h>

// =============================================================================
// Global State and Configuration
// =============================================================================

static ErrorTransformer* g_global_transformer = NULL;
static ErrorCodeRegistry* g_global_code_registry = NULL;
static ErrorTransformationConfig g_config = {
    .enable_auto_conversion = true,
    .enable_message_caching = true,
    .enable_template_caching = true,
    .enable_similarity_analysis = true,
    .max_conversion_chain_length = 10,
    .message_cache_size = 1000,
    .template_cache_size = 500,
    .default_language = ERROR_LANG_EN,
    .enable_fallback_language = true,
    .enable_cultural_adaptations = false
};

static pthread_mutex_t g_transformation_mutex = PTHREAD_MUTEX_INITIALIZER;
static ErrorTransformationStats g_stats = {0};

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static char* safe_strdup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

static int hash_string(const char* str) {
    if (!str) return 0;
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return (int)(hash % INT32_MAX);
}

// =============================================================================
// Error Code Registry Implementation
// =============================================================================

ErrorCodeRegistry* error_code_registry_new(void) {
    ErrorCodeRegistry* registry = calloc(1, sizeof(ErrorCodeRegistry));
    if (!registry) return NULL;
    
    registry->code_capacity = 100;
    registry->codes = calloc(registry->code_capacity, sizeof(MachineErrorCode));
    if (!registry->codes) {
        free(registry);
        return NULL;
    }
    
    // Initialize lookup tables
    registry->code_lookup.codes = calloc(registry->code_capacity, sizeof(int));
    registry->code_lookup.entries = calloc(registry->code_capacity, sizeof(MachineErrorCode*));
    registry->identifier_lookup.identifiers = calloc(registry->code_capacity, sizeof(char*));
    registry->identifier_lookup.entries = calloc(registry->code_capacity, sizeof(MachineErrorCode*));
    
    if (!registry->code_lookup.codes || !registry->code_lookup.entries ||
        !registry->identifier_lookup.identifiers || !registry->identifier_lookup.entries) {
        error_code_registry_free(registry);
        return NULL;
    }
    
    return registry;
}

void error_code_registry_free(ErrorCodeRegistry* registry) {
    if (!registry) return;
    
    // Free all registered codes
    for (int i = 0; i < registry->code_count; i++) {
        MachineErrorCode* code = &registry->codes[i];
        free((void*)code->identifier);
        free((void*)code->category);
        free((void*)code->component);
        free((void*)code->operation);
        free((void*)code->variant_name);
    }
    
    // Free lookup tables
    free(registry->codes);
    free(registry->code_lookup.codes);
    free(registry->code_lookup.entries);
    
    for (int i = 0; i < registry->identifier_lookup.lookup_count; i++) {
        free(registry->identifier_lookup.identifiers[i]);
    }
    free(registry->identifier_lookup.identifiers);
    free(registry->identifier_lookup.entries);
    
    free(registry);
}

MachineErrorCode* error_code_register(ErrorCodeRegistry* registry,
                                     int code,
                                     ErrorCodeClass class,
                                     const char* identifier,
                                     const char* category,
                                     ErrorSeverity severity) {
    if (!registry || !identifier || !category) return NULL;
    
    // Check if we need to expand the registry
    if (registry->code_count >= registry->code_capacity) {
        int new_capacity = registry->code_capacity * 2;
        MachineErrorCode* new_codes = realloc(registry->codes, 
                                            new_capacity * sizeof(MachineErrorCode));
        if (!new_codes) return NULL;
        
        registry->codes = new_codes;
        registry->code_capacity = new_capacity;
        
        // Expand lookup tables too
        int* tmp_codes = realloc(registry->code_lookup.codes,
                                 new_capacity * sizeof(int));
        MachineErrorCode** tmp_code_entries = realloc(registry->code_lookup.entries,
                                                      new_capacity * sizeof(MachineErrorCode*));
        char** tmp_identifiers = realloc(registry->identifier_lookup.identifiers,
                                         new_capacity * sizeof(char*));
        MachineErrorCode** tmp_id_entries = realloc(registry->identifier_lookup.entries,
                                                    new_capacity * sizeof(MachineErrorCode*));
        if (!tmp_codes || !tmp_code_entries || !tmp_identifiers || !tmp_id_entries) {
            if (tmp_codes) registry->code_lookup.codes = tmp_codes;
            if (tmp_code_entries) registry->code_lookup.entries = tmp_code_entries;
            if (tmp_identifiers) registry->identifier_lookup.identifiers = tmp_identifiers;
            if (tmp_id_entries) registry->identifier_lookup.entries = tmp_id_entries;
            return NULL;
        }
        registry->code_lookup.codes = tmp_codes;
        registry->code_lookup.entries = tmp_code_entries;
        registry->identifier_lookup.identifiers = tmp_identifiers;
        registry->identifier_lookup.entries = tmp_id_entries;
    }
    
    // Initialize the new error code
    MachineErrorCode* error_code = &registry->codes[registry->code_count];
    memset(error_code, 0, sizeof(MachineErrorCode));
    
    error_code->code = code;
    error_code->class = class;
    error_code->identifier = safe_strdup(identifier);
    error_code->category = safe_strdup(category);
    error_code->severity = severity;
    error_code->is_recoverable = (severity <= ERROR_SEVERITY_WARNING);
    error_code->is_user_error = false;  // Default to system error
    
    // Add to lookup tables
    registry->code_lookup.codes[registry->code_lookup.lookup_count] = code;
    registry->code_lookup.entries[registry->code_lookup.lookup_count] = error_code;
    registry->code_lookup.lookup_count++;
    
    registry->identifier_lookup.identifiers[registry->identifier_lookup.lookup_count] = safe_strdup(identifier);
    registry->identifier_lookup.entries[registry->identifier_lookup.lookup_count] = error_code;
    registry->identifier_lookup.lookup_count++;
    
    registry->code_count++;
    registry->stats.codes_registered++;
    
    return error_code;
}

MachineErrorCode* error_code_find_by_code(const ErrorCodeRegistry* registry, int code) {
    if (!registry) return NULL;
    
    // Linear search in lookup table (could be optimized with binary search)
    for (int i = 0; i < registry->code_lookup.lookup_count; i++) {
        if (registry->code_lookup.codes[i] == code) {
            ((ErrorCodeRegistry*)registry)->stats.lookups_performed++;
            return registry->code_lookup.entries[i];
        }
    }
    
    return NULL;
}

MachineErrorCode* error_code_find_by_identifier(const ErrorCodeRegistry* registry, 
                                               const char* identifier) {
    if (!registry || !identifier) return NULL;
    
    // Linear search in identifier lookup table
    for (int i = 0; i < registry->identifier_lookup.lookup_count; i++) {
        if (strcmp(registry->identifier_lookup.identifiers[i], identifier) == 0) {
            ((ErrorCodeRegistry*)registry)->stats.lookups_performed++;
            return registry->identifier_lookup.entries[i];
        }
    }
    
    return NULL;
}

void error_code_associate_structured_type(MachineErrorCode* error_code,
                                         ErrorTypeDefinition* type_def,
                                         const char* variant_name) {
    if (!error_code || !type_def) return;
    
    error_code->structured_type = type_def;
    if (variant_name) {
        free((void*)error_code->variant_name);
        error_code->variant_name = safe_strdup(variant_name);
    }
}

// =============================================================================
// Error Localization Context Implementation
// =============================================================================

ErrorLocalizationContext* error_localization_context_new(ErrorLanguage language,
                                                         const char* locale) {
    ErrorLocalizationContext* context = calloc(1, sizeof(ErrorLocalizationContext));
    if (!context) return NULL;
    
    context->language = language;
    context->locale = safe_strdup(locale ? locale : "en_US");
    context->timezone = safe_strdup("UTC");
    
    // Set default formatting preferences
    context->date_format = safe_strdup("%Y-%m-%d");
    context->time_format = safe_strdup("%H:%M:%S");
    context->number_format = safe_strdup("%.2f");
    
    context->use_formal_language = false;
    context->include_cultural_context = false;
    
    return context;
}

void error_localization_context_free(ErrorLocalizationContext* context) {
    if (!context) return;
    
    free((void*)context->locale);
    free((void*)context->timezone);
    free((void*)context->date_format);
    free((void*)context->time_format);
    free((void*)context->number_format);
    
    // Free cached templates
    for (int i = 0; i < context->template_cache.template_count; i++) {
        // Note: We don't own the templates, just the cache array
    }
    free(context->template_cache.templates);
    
    free(context);
}

// =============================================================================
// Error Transformer Implementation
// =============================================================================

ErrorTransformer* error_transformer_new(void) {
    ErrorTransformer* transformer = calloc(1, sizeof(ErrorTransformer));
    if (!transformer) return NULL;
    
    // Initialize conversion rules
    transformer->rule_capacity = 50;
    transformer->rules = calloc(transformer->rule_capacity, sizeof(ErrorConversionRule));
    if (!transformer->rules) {
        free(transformer);
        return NULL;
    }
    
    // Initialize message templates
    transformer->template_capacity = 100;
    transformer->templates = calloc(transformer->template_capacity, sizeof(ErrorMessageTemplate));
    if (!transformer->templates) {
        free(transformer->rules);
        free(transformer);
        return NULL;
    }
    
    // Initialize localization contexts
    transformer->contexts = calloc(10, sizeof(ErrorLocalizationContext));  // Support for 10 languages initially
    transformer->default_language = ERROR_LANG_EN;
    
    // Set default configuration
    transformer->config.auto_convert_compatible = true;
    transformer->config.preserve_original_error = true;
    transformer->config.generate_stack_trace = false;
    transformer->config.max_conversion_depth = 5;
    
    return transformer;
}

void error_transformer_free(ErrorTransformer* transformer) {
    if (!transformer) return;
    
    // Free conversion rules
    for (int i = 0; i < transformer->rule_count; i++) {
        ErrorConversionRule* rule = &transformer->rules[i];
        free(rule->field_mappings);
        free((void*)rule->rule_name);
        free((void*)rule->description);
    }
    free(transformer->rules);
    
    // Free message templates
    for (int i = 0; i < transformer->template_count; i++) {
        ErrorMessageTemplate* template = &transformer->templates[i];
        free((void*)template->template_id);
        free((void*)template->error_identifier);
        free((void*)template->short_message);
        free((void*)template->detailed_message);
        free((void*)template->user_message);
        free((void*)template->technical_message);
        free((void*)template->recovery_suggestion);
        
        if (template->placeholder_names) {
            for (int j = 0; j < template->placeholder_count; j++) {
                free((void*)template->placeholder_names[j]);
            }
            free(template->placeholder_names);
        }
        
        free((void*)template->context_variations.cli_message);
        free((void*)template->context_variations.web_message);
        free((void*)template->context_variations.api_message);
        free((void*)template->context_variations.log_message);
    }
    free(transformer->templates);
    
    // Free localization contexts
    for (int i = 0; i < transformer->context_count; i++) {
        error_localization_context_free(&transformer->contexts[i]);
    }
    free(transformer->contexts);
    
    free(transformer);
}

void error_transformer_add_conversion_rule(ErrorTransformer* transformer,
                                          ErrorConversionRule* rule) {
    if (!transformer || !rule) return;
    
    // Check if we need to expand rules array
    if (transformer->rule_count >= transformer->rule_capacity) {
        int new_capacity = transformer->rule_capacity * 2;
        ErrorConversionRule* new_rules = realloc(transformer->rules,
                                                new_capacity * sizeof(ErrorConversionRule));
        if (!new_rules) return;
        
        transformer->rules = new_rules;
        transformer->rule_capacity = new_capacity;
    }
    
    // Copy the rule
    transformer->rules[transformer->rule_count] = *rule;
    transformer->rule_count++;
}

ErrorConversionRule* error_transformer_find_conversion_rule(const ErrorTransformer* transformer,
                                                          const StructuredError* source_error,
                                                          ErrorTypeDefinition* target_type) {
    if (!transformer || !source_error || !target_type) return NULL;
    
    ErrorConversionRule* best_rule = NULL;
    double best_cost = INFINITY;
    
    // Search through all rules to find the best match
    for (int i = 0; i < transformer->rule_count; i++) {
        ErrorConversionRule* rule = &transformer->rules[i];
        
        // Check if source type matches
        if (rule->source_type != source_error->type_def) {
            continue;
        }
        
        // Check if target type matches
        if (rule->target_type != target_type) {
            continue;
        }
        
        // Check variant if specified
        if (rule->source_variant && 
            strcmp(rule->source_variant, source_error->variant->name) != 0) {
            continue;
        }
        
        // This rule matches - check if it's better than our current best
        if (rule->conversion_cost < best_cost) {
            best_rule = &transformer->rules[i];
            best_cost = rule->conversion_cost;
        }
    }
    
    return best_rule;
}

StructuredError* error_transformer_convert(ErrorTransformer* transformer,
                                         const StructuredError* source_error,
                                         ErrorTypeDefinition* target_type) {
    if (!transformer || !source_error || !target_type) return NULL;
    
    uint64_t start_time = get_current_time_ms();
    
    // Find conversion rule
    ErrorConversionRule* rule = error_transformer_find_conversion_rule(transformer,
                                                                      source_error,
                                                                      target_type);
    if (!rule) return NULL;
    
    StructuredError* converted_error = NULL;
    
    // Use custom converter if available
    if (rule->converter) {
        converted_error = rule->converter(source_error, rule->converter_context);
    } else {
        // Perform default field mapping conversion
        const char* target_variant = rule->target_variant ? rule->target_variant : "GenericError";
        converted_error = structured_error_new(target_type, target_variant);
        if (!converted_error) return NULL;
        
        // Map fields according to the rule
        for (int i = 0; i < rule->field_mapping_count; i++) {
            const char* source_field = rule->field_mappings[i].source_field;
            const char* target_field = rule->field_mappings[i].target_field;
            
            // Get value from source field
            const char* string_value;
            int64_t int_value;
            double float_value;
            bool bool_value;
            
            if (structured_error_get_string_field(source_error, source_field, &string_value)) {
                structured_error_set_string_field(converted_error, target_field, string_value);
            } else if (structured_error_get_int_field(source_error, source_field, &int_value)) {
                structured_error_set_int_field(converted_error, target_field, int_value);
            } else if (structured_error_get_float_field(source_error, source_field, &float_value)) {
                structured_error_set_float_field(converted_error, target_field, float_value);
            } else if (structured_error_get_bool_field(source_error, source_field, &bool_value)) {
                structured_error_set_bool_field(converted_error, target_field, bool_value);
            }
        }
        
        // Preserve original error if configured
        if (transformer->config.preserve_original_error) {
            // Note: In a real implementation, we might store this in a separate field
            // For now, we'll just copy some basic information
            converted_error->severity = source_error->severity;
            converted_error->location = source_error->location;
        }
        
        structured_error_finalize(converted_error);
    }
    
    // Update statistics
    rule->usage_count++;
    transformer->stats.conversions_performed++;
    
    uint64_t end_time = get_current_time_ms();
    double duration = (double)(end_time - start_time);
    transformer->stats.conversions_performed++;
    
    // Update rolling average (simple exponential moving average)
    static double avg_time = 0.0;
    avg_time = avg_time * 0.9 + duration * 0.1;
    
    return converted_error;
}

bool error_transformer_can_convert(const ErrorTransformer* transformer,
                                  const StructuredError* source_error,
                                  ErrorTypeDefinition* target_type) {
    return error_transformer_find_conversion_rule(transformer, source_error, target_type) != NULL;
}

// =============================================================================
// Message Generation Implementation
// =============================================================================

void error_transformer_add_message_template(ErrorTransformer* transformer,
                                           ErrorMessageTemplate* template) {
    if (!transformer || !template) return;
    
    // Check if we need to expand templates array
    if (transformer->template_count >= transformer->template_capacity) {
        int new_capacity = transformer->template_capacity * 2;
        ErrorMessageTemplate* new_templates = realloc(transformer->templates,
                                                     new_capacity * sizeof(ErrorMessageTemplate));
        if (!new_templates) return;
        
        transformer->templates = new_templates;
        transformer->template_capacity = new_capacity;
    }
    
    // Copy the template
    transformer->templates[transformer->template_count] = *template;
    transformer->template_count++;
}

ErrorMessageTemplate* error_transformer_find_template(const ErrorTransformer* transformer,
                                                     const char* error_identifier,
                                                     ErrorLanguage language) {
    if (!transformer || !error_identifier) return NULL;
    
    // For now, we'll ignore language and just find by identifier
    // In a full implementation, we'd have language-specific templates
    for (int i = 0; i < transformer->template_count; i++) {
        ErrorMessageTemplate* template = &transformer->templates[i];
        if (strcmp(template->error_identifier, error_identifier) == 0) {
            return template;
        }
    }
    
    return NULL;
}

char* error_replace_placeholders(const char* template_str,
                                const StructuredError* error,
                                const ErrorLocalizationContext* context) {
    if (!template_str || !error) return safe_strdup(template_str);
    
    // Simple placeholder replacement implementation
    // In a real implementation, this would be much more sophisticated
    size_t template_len = strlen(template_str);
    size_t result_capacity = template_len * 2;  // Start with double the template size
    char* result = malloc(result_capacity);
    if (!result) return NULL;
    
    size_t result_len = 0;
    const char* pos = template_str;
    
    while (*pos) {
        if (*pos == '{' && *(pos + 1) == '{') {
            // Find the end of the placeholder
            const char* end = strstr(pos + 2, "}}");
            if (end) {
                // Extract placeholder name
                size_t placeholder_len = end - pos - 2;
                char* placeholder = malloc(placeholder_len + 1);
                if (placeholder) {
                    memcpy(placeholder, pos + 2, placeholder_len);
                    placeholder[placeholder_len] = '\0';
                    
                    // Look up the field value
                    const char* replacement = NULL;
                    int64_t int_val;
                    double float_val;
                    bool bool_val;
                    
                    if (structured_error_get_string_field(error, placeholder, &replacement)) {
                        // Use string value directly
                    } else if (structured_error_get_int_field(error, placeholder, &int_val)) {
                        // Convert int to string
                        static char int_buffer[32];
                        snprintf(int_buffer, sizeof(int_buffer), "%ld", int_val);
                        replacement = int_buffer;
                    } else if (structured_error_get_float_field(error, placeholder, &float_val)) {
                        // Convert float to string
                        static char float_buffer[32];
                        snprintf(float_buffer, sizeof(float_buffer), "%.2f", float_val);
                        replacement = float_buffer;
                    } else if (structured_error_get_bool_field(error, placeholder, &bool_val)) {
                        replacement = bool_val ? "true" : "false";
                    } else {
                        replacement = "[unknown]";
                    }
                    
                    // Ensure we have enough space
                    size_t replacement_len = strlen(replacement);
                    if (result_len + replacement_len >= result_capacity) {
                        result_capacity = (result_len + replacement_len + 1) * 2;
                        char* new_result = realloc(result, result_capacity);
                        if (!new_result) {
                            free(placeholder);
                            free(result);
                            return NULL;
                        }
                        result = new_result;
                    }
                    
                    // Copy replacement
                    memcpy(result + result_len, replacement, replacement_len);
                    result_len += replacement_len;
                    
                    free(placeholder);
                }
                
                pos = end + 2;  // Skip past "}}"
            } else {
                // Malformed placeholder, just copy the character
                result[result_len++] = *pos++;
            }
        } else {
            // Regular character
            if (result_len >= result_capacity - 1) {
                result_capacity *= 2;
                char* new_result = realloc(result, result_capacity);
                if (!new_result) {
                    free(result);
                    return NULL;
                }
                result = new_result;
            }
            result[result_len++] = *pos++;
        }
    }
    
    result[result_len] = '\0';
    return result;
}

char* error_transformer_generate_message(ErrorTransformer* transformer,
                                        const StructuredError* error,
                                        const ErrorLocalizationContext* context,
                                        const char* message_type) {
    if (!transformer || !error || !message_type) return NULL;
    
    uint64_t start_time = get_current_time_ms();
    
    // Find appropriate template
    const char* error_identifier = error->type_def->name;  // Simplified
    ErrorLanguage language = context ? context->language : transformer->default_language;
    
    ErrorMessageTemplate* template = error_transformer_find_template(transformer,
                                                                    error_identifier,
                                                                    language);
    if (!template) {
        // Generate a generic message
        char* generic_msg = malloc(256);
        if (generic_msg) {
            snprintf(generic_msg, 256, "Error in %s: %s",
                    error->type_def->name,
                    error->variant->name);
        }
        return generic_msg;
    }
    
    // Select appropriate template based on message type
    const char* template_str = NULL;
    if (strcmp(message_type, "short") == 0) {
        template_str = template->short_message;
    } else if (strcmp(message_type, "detailed") == 0) {
        template_str = template->detailed_message;
    } else if (strcmp(message_type, "user") == 0) {
        template_str = template->user_message;
    } else if (strcmp(message_type, "technical") == 0) {
        template_str = template->technical_message;
    } else if (strcmp(message_type, "recovery") == 0) {
        template_str = template->recovery_suggestion;
    } else {
        template_str = template->short_message;  // Default
    }
    
    if (!template_str) {
        template_str = "An error occurred";
    }
    
    // Replace placeholders
    char* message = error_replace_placeholders(template_str, error, context);
    
    // Update statistics
    transformer->stats.messages_generated++;
    
    uint64_t end_time = get_current_time_ms();
    double duration = (double)(end_time - start_time);
    
    return message;
}

char* error_transformer_generate_localized_message(ErrorTransformer* transformer,
                                                  const StructuredError* error,
                                                  ErrorLanguage language,
                                                  const char* message_type) {
    if (!transformer || !error || !message_type) return NULL;
    
    // Create a temporary localization context
    ErrorLocalizationContext* context = error_localization_context_new(language, NULL);
    if (!context) return NULL;
    
    char* message = error_transformer_generate_message(transformer, error, context, message_type);
    
    error_localization_context_free(context);
    return message;
}

// =============================================================================
// Built-in Templates and Conversion Rules
// =============================================================================

ErrorMessageTemplate* create_file_not_found_template(void) {
    ErrorMessageTemplate* template = calloc(1, sizeof(ErrorMessageTemplate));
    if (!template) return NULL;
    
    template->template_id = safe_strdup("FILE_NOT_FOUND");
    template->error_identifier = safe_strdup("ConfigError");
    template->short_message = safe_strdup("File not found: {{path}}");
    template->detailed_message = safe_strdup("The configuration file '{{path}}' could not be found. Please check the file path and permissions.");
    template->user_message = safe_strdup("Configuration file not found. Please check that the file exists and you have permission to read it.");
    template->technical_message = safe_strdup("FileNotFound: {{path}} (errno: 2, ENOENT)");
    template->recovery_suggestion = safe_strdup("Check the file path and ensure the file exists. You may need to create a default configuration file.");
    
    // Context variations
    template->context_variations.cli_message = safe_strdup("Error: Configuration file '{{path}}' not found. Use --config to specify a different path.");
    template->context_variations.web_message = safe_strdup("Configuration file not found. Please contact your system administrator.");
    template->context_variations.api_message = safe_strdup("{\"error\": \"file_not_found\", \"path\": \"{{path}}\", \"code\": 2001}");
    template->context_variations.log_message = safe_strdup("[ERROR] ConfigurationLoader: File not found: {{path}}");
    
    return template;
}

ErrorMessageTemplate* create_network_timeout_template(void) {
    ErrorMessageTemplate* template = calloc(1, sizeof(ErrorMessageTemplate));
    if (!template) return NULL;
    
    template->template_id = safe_strdup("NETWORK_TIMEOUT");
    template->error_identifier = safe_strdup("NetworkError");
    template->short_message = safe_strdup("Connection timeout to {{host}}:{{port}}");
    template->detailed_message = safe_strdup("Failed to connect to {{host}}:{{port}} within {{timeout_ms}}ms. The server may be unavailable or the network may be slow.");
    template->user_message = safe_strdup("Unable to connect to the server. Please check your internet connection and try again.");
    template->technical_message = safe_strdup("ConnectionTimeout: {{host}}:{{port}} timeout={{timeout_ms}}ms");
    template->recovery_suggestion = safe_strdup("Check your network connection, verify the server address, or try increasing the timeout value.");
    
    return template;
}

ErrorConversionRule* create_system_to_user_conversion_rule(void) {
    ErrorConversionRule* rule = calloc(1, sizeof(ErrorConversionRule));
    if (!rule) return NULL;
    
    // This would be set up with appropriate type definitions in a real implementation
    rule->compatibility = ERROR_CONVERSION_LOSSY;
    rule->conversion_cost = 0.3;  // Moderate cost due to information loss
    rule->rule_name = safe_strdup("SystemToUserError");
    rule->description = safe_strdup("Convert technical system errors to user-friendly errors");
    
    return rule;
}

void error_transformer_register_standard_templates(ErrorTransformer* transformer) {
    if (!transformer) return;
    
    ErrorMessageTemplate* file_template = create_file_not_found_template();
    if (file_template) error_transformer_add_message_template(transformer, file_template);
    
    ErrorMessageTemplate* network_template = create_network_timeout_template();
    if (network_template) error_transformer_add_message_template(transformer, network_template);
}

void error_transformer_register_standard_conversions(ErrorTransformer* transformer) {
    if (!transformer) return;
    
    ErrorConversionRule* system_to_user = create_system_to_user_conversion_rule();
    if (system_to_user) error_transformer_add_conversion_rule(transformer, system_to_user);
}

// =============================================================================
// System Initialization and Management
// =============================================================================

void error_transformation_system_init(void) {
    pthread_mutex_lock(&g_transformation_mutex);
    
    if (!g_global_transformer) {
        g_global_transformer = error_transformer_new();
        if (g_global_transformer) {
            error_transformer_register_standard_conversions(g_global_transformer);
            error_transformer_register_standard_templates(g_global_transformer);
        }
    }
    
    if (!g_global_code_registry) {
        g_global_code_registry = error_code_registry_new();
        if (g_global_code_registry) {
            // Register some standard error codes
            error_code_register(g_global_code_registry, 2001, ERROR_CODE_CLASS_CONFIG,
                              "CONFIG_FILE_NOT_FOUND", "filesystem", ERROR_SEVERITY_ERROR);
            error_code_register(g_global_code_registry, 3001, ERROR_CODE_CLASS_NETWORK,
                              "NETWORK_TIMEOUT", "network", ERROR_SEVERITY_ERROR);
            error_code_register(g_global_code_registry, 3002, ERROR_CODE_CLASS_NETWORK,
                              "CONNECTION_REFUSED", "network", ERROR_SEVERITY_ERROR);
        }
    }
    
    pthread_mutex_unlock(&g_transformation_mutex);
}

void error_transformation_system_shutdown(void) {
    pthread_mutex_lock(&g_transformation_mutex);
    
    if (g_global_transformer) {
        error_transformer_free(g_global_transformer);
        g_global_transformer = NULL;
    }
    
    if (g_global_code_registry) {
        error_code_registry_free(g_global_code_registry);
        g_global_code_registry = NULL;
    }
    
    pthread_mutex_unlock(&g_transformation_mutex);
}

ErrorTransformer* get_global_error_transformer(void) {
    return g_global_transformer;
}

// =============================================================================
// Language and Locale Utilities
// =============================================================================

const char* error_language_to_string(ErrorLanguage language) {
    switch (language) {
        case ERROR_LANG_EN: return "en";
        case ERROR_LANG_ES: return "es";
        case ERROR_LANG_FR: return "fr";
        case ERROR_LANG_DE: return "de";
        case ERROR_LANG_IT: return "it";
        case ERROR_LANG_PT: return "pt";
        case ERROR_LANG_RU: return "ru";
        case ERROR_LANG_ZH: return "zh";
        case ERROR_LANG_JA: return "ja";
        case ERROR_LANG_KO: return "ko";
        case ERROR_LANG_CUSTOM: return "custom";
        default: return "unknown";
    }
}

ErrorLanguage error_language_from_string(const char* language_str) {
    if (!language_str) return ERROR_LANG_EN;
    
    if (strcmp(language_str, "en") == 0) return ERROR_LANG_EN;
    if (strcmp(language_str, "es") == 0) return ERROR_LANG_ES;
    if (strcmp(language_str, "fr") == 0) return ERROR_LANG_FR;
    if (strcmp(language_str, "de") == 0) return ERROR_LANG_DE;
    if (strcmp(language_str, "it") == 0) return ERROR_LANG_IT;
    if (strcmp(language_str, "pt") == 0) return ERROR_LANG_PT;
    if (strcmp(language_str, "ru") == 0) return ERROR_LANG_RU;
    if (strcmp(language_str, "zh") == 0) return ERROR_LANG_ZH;
    if (strcmp(language_str, "ja") == 0) return ERROR_LANG_JA;
    if (strcmp(language_str, "ko") == 0) return ERROR_LANG_KO;
    
    return ERROR_LANG_EN;  // Default fallback
}

// =============================================================================
// Debug and Utility Functions
// =============================================================================

void print_error_transformation_stats(const ErrorTransformer* transformer) {
    if (!transformer) return;
    
    printf("🔄 Error Transformation Statistics\n");
    printf("═══════════════════════════════════\n");
    printf("Transformations:\n");
    printf("  Conversions Performed:     %lu\n", transformer->stats.conversions_performed);
    printf("  Messages Generated:        %lu\n", transformer->stats.messages_generated);
    printf("  Template Cache Hits:       %lu\n", transformer->stats.cache_hits);
    printf("  Template Cache Misses:     %lu\n", transformer->stats.cache_misses);
    
    printf("\nConfiguration:\n");
    printf("  Conversion Rules:          %d\n", transformer->rule_count);
    printf("  Message Templates:         %d\n", transformer->template_count);
    printf("  Localization Contexts:     %d\n", transformer->context_count);
    printf("  Default Language:          %s\n", error_language_to_string(transformer->default_language));
    
    if (transformer->stats.conversions_performed > 0) {
        printf("\nPerformance:\n");
        printf("  Avg Conversion Time:       %.2f ms\n", 0.0);  // Would be calculated in real implementation
        printf("  Avg Message Gen Time:      %.2f ms\n", 0.0);  // Would be calculated in real implementation
    }
}

void print_error_code_registry(const ErrorCodeRegistry* registry) {
    if (!registry) return;
    
    printf("📋 Error Code Registry\n");
    printf("════════════════════════\n");
    printf("Registered Codes: %d\n", registry->code_count);
    printf("Lookups Performed: %lu\n", registry->stats.lookups_performed);
    printf("\nError Codes:\n");
    
    for (int i = 0; i < registry->code_count; i++) {
        const MachineErrorCode* code = &registry->codes[i];
        printf("  %d: %s (%s) - %s\n",
               code->code,
               code->identifier,
               code->category,
               error_severity_to_string(code->severity));
    }
}

ErrorTransformationStats get_error_transformation_stats(void) {
    return g_stats;
}

void print_error_transformation_performance_report(void) {
    ErrorTransformationStats stats = get_error_transformation_stats();
    
    printf("📊 Error Transformation Performance Report\n");
    printf("══════════════════════════════════════════\n");
    printf("Total Transformations:     %lu\n", stats.transformations_performed);
    printf("Total Messages Generated:  %lu\n", stats.messages_generated);
    printf("Template Cache Hit Rate:   %.1f%%\n", 
           stats.template_cache_hits + stats.template_cache_misses > 0 ?
           (double)stats.template_cache_hits * 100.0 / 
           (stats.template_cache_hits + stats.template_cache_misses) : 0.0);
    printf("Conversion Cache Hit Rate: %.1f%%\n",
           stats.conversion_cache_hits + stats.conversion_cache_misses > 0 ?
           (double)stats.conversion_cache_hits * 100.0 /
           (stats.conversion_cache_hits + stats.conversion_cache_misses) : 0.0);
    
    if (g_global_transformer) {
        printf("\nGlobal Transformer:\n");
        print_error_transformation_stats(g_global_transformer);
    }
    
    if (g_global_code_registry) {
        printf("\nGlobal Code Registry:\n");
        print_error_code_registry(g_global_code_registry);
    }
}

// =============================================================================
// Configuration
// =============================================================================

void configure_error_transformation_system(const ErrorTransformationConfig* config) {
    if (!config) return;
    
    pthread_mutex_lock(&g_transformation_mutex);
    g_config = *config;
    pthread_mutex_unlock(&g_transformation_mutex);
}

ErrorTransformationConfig* get_error_transformation_config(void) {
    return &g_config;
}