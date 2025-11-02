#define _POSIX_C_SOURCE 200809L
#include "error_hierarchies.h"
#include "error_aggregation.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <regex.h>
#include <unistd.h>
#include <math.h>

// =============================================================================
// Global State and Configuration
// =============================================================================

static ErrorHierarchy* g_global_hierarchy = NULL;
static ErrorHierarchyConfig g_config = {
    .enable_inheritance = true,
    .enable_pattern_matching = true,
    .enable_json_serialization = false,
    .enable_statistics = true,
    .initial_type_capacity = 64,
    .initial_error_capacity = 256,
    .cache_type_lookups = true,
    .cache_inheritance_queries = true,
    .max_inheritance_depth = 10
};

static ErrorHierarchyStats g_stats = {0};
static pthread_mutex_t g_hierarchy_mutex = PTHREAD_MUTEX_INITIALIZER;

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t generate_type_id(const char* name) {
    // Simple hash function for type IDs
    uint64_t hash = 5381;
    for (const char* p = name; *p; p++) {
        hash = ((hash << 5) + hash) + *p;
    }
    return hash;
}

static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char* duplicate_string(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

static uint64_t get_thread_id(void) {
    return (uint64_t)pthread_self();
}

// =============================================================================
// Error Hierarchy System Lifecycle
// =============================================================================

void error_hierarchy_system_init(void) {
    pthread_mutex_lock(&g_hierarchy_mutex);
    
    if (g_global_hierarchy) {
        pthread_mutex_unlock(&g_hierarchy_mutex);
        return; // Already initialized
    }
    
    g_global_hierarchy = calloc(1, sizeof(ErrorHierarchy));
    if (!g_global_hierarchy) {
        pthread_mutex_unlock(&g_hierarchy_mutex);
        return;
    }
    
    // Initialize type registry
    g_global_hierarchy->type_capacity = g_config.initial_type_capacity;
    g_global_hierarchy->registered_types = calloc(g_global_hierarchy->type_capacity, 
                                                  sizeof(ErrorTypeDefinition*));
    
    // Initialize type lookup table
    g_global_hierarchy->type_lookup.type_ids = calloc(g_global_hierarchy->type_capacity,
                                                     sizeof(uint64_t));
    g_global_hierarchy->type_lookup.type_defs = calloc(g_global_hierarchy->type_capacity,
                                                      sizeof(ErrorTypeDefinition*));
    
    // Initialize inheritance table
    g_global_hierarchy->inheritance_table = calloc(g_global_hierarchy->type_capacity,
                                                  sizeof(*g_global_hierarchy->inheritance_table));
    
    // Reset statistics
    memset(&g_stats, 0, sizeof(g_stats));
    
    pthread_mutex_unlock(&g_hierarchy_mutex);
    
    printf("🏗️  Error hierarchy system initialized\n");
}

void error_hierarchy_system_shutdown(void) {
    pthread_mutex_lock(&g_hierarchy_mutex);
    
    if (!g_global_hierarchy) {
        pthread_mutex_unlock(&g_hierarchy_mutex);
        return;
    }
    
    // Free all registered types
    for (int i = 0; i < g_global_hierarchy->type_count; i++) {
        ErrorTypeDefinition* type_def = g_global_hierarchy->registered_types[i];
        if (type_def) {
            // Free variants
            for (int j = 0; j < type_def->variant_count; j++) {
                ErrorVariant* variant = &type_def->variants[j];
                
                // Free fields
                for (int k = 0; k < variant->field_count; k++) {
                    ErrorField* field = &variant->fields[k];
                    free((void*)field->name);
                    free((void*)field->description);
                    
                    // Free type-specific constraints
                    if (field->type == ERROR_FIELD_STRING && field->string_constraints.pattern) {
                        free((void*)field->string_constraints.pattern);
                    } else if (field->type == ERROR_FIELD_ENUM && field->enum_constraints.valid_values) {
                        for (int l = 0; l < field->enum_constraints.value_count; l++) {
                            free((void*)field->enum_constraints.valid_values[l]);
                        }
                        free(field->enum_constraints.valid_values);
                    }
                }
                free(variant->fields);
                
                // Free recovery actions
                if (variant->recovery_actions) {
                    for (int k = 0; k < variant->recovery_action_count; k++) {
                        free((void*)variant->recovery_actions[k]);
                    }
                    free(variant->recovery_actions);
                }
                
                free((void*)variant->name);
                free((void*)variant->description);
                free((void*)variant->format_template);
                free((void*)variant->hint_template);
            }
            free(type_def->variants);
            
            // Free children array
            free(type_def->children);
            
            free((void*)type_def->name);
            free((void*)type_def->description);
            free((void*)type_def->namespace);
            free((void*)type_def->source_file);
            free(type_def);
        }
    }
    
    // Free arrays
    free(g_global_hierarchy->registered_types);
    free(g_global_hierarchy->type_lookup.type_ids);
    free(g_global_hierarchy->type_lookup.type_defs);
    free(g_global_hierarchy->inheritance_table);
    
    free(g_global_hierarchy);
    g_global_hierarchy = NULL;
    
    pthread_mutex_unlock(&g_hierarchy_mutex);
    
    printf("🏗️  Error hierarchy system shutdown\n");
}

ErrorHierarchy* get_global_error_hierarchy(void) {
    return g_global_hierarchy;
}

// =============================================================================
// Error Type Registration and Management
// =============================================================================

ErrorTypeDefinition* error_type_define(const char* name, const char* description) {
    if (!name || !g_global_hierarchy) return NULL;
    
    pthread_mutex_lock(&g_hierarchy_mutex);
    
    // Check if type already exists
    for (int i = 0; i < g_global_hierarchy->type_count; i++) {
        if (strcmp(g_global_hierarchy->registered_types[i]->name, name) == 0) {
            pthread_mutex_unlock(&g_hierarchy_mutex);
            return g_global_hierarchy->registered_types[i];
        }
    }
    
    // Expand capacity if needed
    if (g_global_hierarchy->type_count >= g_global_hierarchy->type_capacity) {
        int new_capacity = g_global_hierarchy->type_capacity * 2;
        
        ErrorTypeDefinition** new_types = realloc(g_global_hierarchy->registered_types,
                                                  new_capacity * sizeof(ErrorTypeDefinition*));
        if (!new_types) {
            pthread_mutex_unlock(&g_hierarchy_mutex);
            return NULL;
        }
        g_global_hierarchy->registered_types = new_types;
        
        // Expand lookup tables
        uint64_t* new_ids = realloc(g_global_hierarchy->type_lookup.type_ids,
                                   new_capacity * sizeof(uint64_t));
        ErrorTypeDefinition** new_defs = realloc(g_global_hierarchy->type_lookup.type_defs,
                                                 new_capacity * sizeof(ErrorTypeDefinition*));
        if (!new_ids || !new_defs) {
            pthread_mutex_unlock(&g_hierarchy_mutex);
            return NULL;
        }
        g_global_hierarchy->type_lookup.type_ids = new_ids;
        g_global_hierarchy->type_lookup.type_defs = new_defs;
        
        // Expand inheritance table
        void* new_inheritance = realloc(g_global_hierarchy->inheritance_table,
                                       new_capacity * sizeof(*g_global_hierarchy->inheritance_table));
        if (!new_inheritance) {
            pthread_mutex_unlock(&g_hierarchy_mutex);
            return NULL;
        }
        g_global_hierarchy->inheritance_table = new_inheritance;
        
        g_global_hierarchy->type_capacity = new_capacity;
    }
    
    // Create new type definition
    ErrorTypeDefinition* type_def = calloc(1, sizeof(ErrorTypeDefinition));
    if (!type_def) {
        pthread_mutex_unlock(&g_hierarchy_mutex);
        return NULL;
    }
    
    type_def->name = duplicate_string(name);
    type_def->description = duplicate_string(description);
    type_def->type_id = generate_type_id(name);
    type_def->variants = NULL;
    type_def->variant_count = 0;
    type_def->parent = NULL;
    type_def->children = NULL;
    type_def->child_count = 0;
    type_def->is_abstract = false;
    type_def->type_destructor = NULL;
    
    // Add to registry
    int index = g_global_hierarchy->type_count;
    g_global_hierarchy->registered_types[index] = type_def;
    
    // Add to lookup table
    g_global_hierarchy->type_lookup.type_ids[index] = type_def->type_id;
    g_global_hierarchy->type_lookup.type_defs[index] = type_def;
    g_global_hierarchy->type_lookup.lookup_count++;
    
    // Initialize inheritance entry
    g_global_hierarchy->inheritance_table[index].parent = NULL;
    g_global_hierarchy->inheritance_table[index].children = NULL;
    g_global_hierarchy->inheritance_table[index].child_count = 0;
    
    g_global_hierarchy->type_count++;
    g_stats.types_registered++;
    
    pthread_mutex_unlock(&g_hierarchy_mutex);
    
    return type_def;
}

ErrorVariant* error_type_add_variant(ErrorTypeDefinition* type_def, 
                                     const char* variant_name,
                                     const char* description,
                                     int error_code) {
    if (!type_def || !variant_name) return NULL;
    
    pthread_mutex_lock(&g_hierarchy_mutex);
    
    // Check if variant already exists
    for (int i = 0; i < type_def->variant_count; i++) {
        if (strcmp(type_def->variants[i].name, variant_name) == 0) {
            pthread_mutex_unlock(&g_hierarchy_mutex);
            return &type_def->variants[i];
        }
    }
    
    // Expand variants array if needed
    ErrorVariant* new_variants = realloc(type_def->variants,
                                        (type_def->variant_count + 1) * sizeof(ErrorVariant));
    if (!new_variants) {
        pthread_mutex_unlock(&g_hierarchy_mutex);
        return NULL;
    }
    type_def->variants = new_variants;
    
    // Initialize new variant
    ErrorVariant* variant = &type_def->variants[type_def->variant_count];
    memset(variant, 0, sizeof(ErrorVariant));
    
    variant->name = duplicate_string(variant_name);
    variant->description = duplicate_string(description);
    variant->error_code = error_code;
    variant->severity = ERROR_SEVERITY_ERROR; // Default severity
    variant->fields = NULL;
    variant->field_count = 0;
    variant->format_template = NULL;
    variant->hint_template = NULL;
    variant->recovery_actions = NULL;
    variant->recovery_action_count = 0;
    
    type_def->variant_count++;
    g_stats.variants_defined++;
    
    pthread_mutex_unlock(&g_hierarchy_mutex);
    
    return variant;
}

// =============================================================================
// Field Definition Functions
// =============================================================================

static void error_variant_add_field(ErrorVariant* variant, const char* name,
                                   const char* description, bool required,
                                   ErrorFieldType type) {
    if (!variant || !name) return;
    
    // Expand fields array
    ErrorField* new_fields = realloc(variant->fields,
                                    (variant->field_count + 1) * sizeof(ErrorField));
    if (!new_fields) return;
    variant->fields = new_fields;
    
    // Initialize new field
    ErrorField* field = &variant->fields[variant->field_count];
    memset(field, 0, sizeof(ErrorField));
    
    field->name = duplicate_string(name);
    field->description = duplicate_string(description);
    field->type = type;
    field->required = required;
    
    variant->field_count++;
}

void error_variant_add_string_field(ErrorVariant* variant, const char* name, 
                                   const char* description, bool required) {
    error_variant_add_field(variant, name, description, required, ERROR_FIELD_STRING);
}

void error_variant_add_int_field(ErrorVariant* variant, const char* name,
                                const char* description, bool required,
                                int64_t min_val, int64_t max_val) {
    error_variant_add_field(variant, name, description, required, ERROR_FIELD_INT);
    
    if (variant->field_count > 0) {
        ErrorField* field = &variant->fields[variant->field_count - 1];
        field->numeric_constraints.min_value = min_val;
        field->numeric_constraints.max_value = max_val;
    }
}

void error_variant_add_float_field(ErrorVariant* variant, const char* name,
                                  const char* description, bool required,
                                  double min_val, double max_val) {
    error_variant_add_field(variant, name, description, required, ERROR_FIELD_FLOAT);
    
    if (variant->field_count > 0) {
        ErrorField* field = &variant->fields[variant->field_count - 1];
        field->numeric_constraints.min_value = (int64_t)min_val;
        field->numeric_constraints.max_value = (int64_t)max_val;
    }
}

void error_variant_add_bool_field(ErrorVariant* variant, const char* name,
                                 const char* description, bool required) {
    error_variant_add_field(variant, name, description, required, ERROR_FIELD_BOOL);
}

void error_variant_add_enum_field(ErrorVariant* variant, const char* name,
                                 const char* description, bool required,
                                 const char** valid_values, int value_count) {
    error_variant_add_field(variant, name, description, required, ERROR_FIELD_ENUM);
    
    if (variant->field_count > 0 && valid_values && value_count > 0) {
        ErrorField* field = &variant->fields[variant->field_count - 1];
        
        // Copy valid values
        field->enum_constraints.valid_values = malloc(value_count * sizeof(char*));
        if (field->enum_constraints.valid_values) {
            for (int i = 0; i < value_count; i++) {
                field->enum_constraints.valid_values[i] = duplicate_string(valid_values[i]);
            }
            field->enum_constraints.value_count = value_count;
        }
    }
}

// =============================================================================
// Type Inheritance Functions
// =============================================================================

void error_type_set_parent(ErrorTypeDefinition* child, ErrorTypeDefinition* parent) {
    if (!child || !parent || !g_global_hierarchy) return;
    
    pthread_mutex_lock(&g_hierarchy_mutex);
    
    // Find child in inheritance table
    int child_index = -1;
    for (int i = 0; i < g_global_hierarchy->type_count; i++) {
        if (g_global_hierarchy->registered_types[i] == child) {
            child_index = i;
            break;
        }
    }
    
    if (child_index == -1) {
        pthread_mutex_unlock(&g_hierarchy_mutex);
        return;
    }
    
    // Set parent relationship
    child->parent = parent;
    g_global_hierarchy->inheritance_table[child_index].parent = parent;
    
    // Add child to parent's children list
    parent->children = realloc(parent->children, 
                              (parent->child_count + 1) * sizeof(ErrorTypeDefinition*));
    if (parent->children) {
        parent->children[parent->child_count] = child;
        parent->child_count++;
    }
    
    pthread_mutex_unlock(&g_hierarchy_mutex);
}

bool error_type_is_subtype_of(ErrorTypeDefinition* child, ErrorTypeDefinition* parent) {
    if (!child || !parent) return false;
    if (child == parent) return true;
    
    pthread_mutex_lock(&g_hierarchy_mutex);
    g_stats.inheritance_queries++;
    
    // Walk up the inheritance chain
    ErrorTypeDefinition* current = child->parent;
    int depth = 0;
    
    while (current && depth < g_config.max_inheritance_depth) {
        if (current == parent) {
            pthread_mutex_unlock(&g_hierarchy_mutex);
            return true;
        }
        current = current->parent;
        depth++;
    }
    
    pthread_mutex_unlock(&g_hierarchy_mutex);
    return false;
}

ErrorTypeDefinition** error_type_get_ancestors(ErrorTypeDefinition* type, int* ancestor_count) {
    if (!type || !ancestor_count) return NULL;
    
    *ancestor_count = 0;
    
    // Count ancestors first
    ErrorTypeDefinition* current = type->parent;
    int count = 0;
    while (current && count < g_config.max_inheritance_depth) {
        count++;
        current = current->parent;
    }
    
    if (count == 0) return NULL;
    
    // Allocate and fill ancestors array
    ErrorTypeDefinition** ancestors = malloc(count * sizeof(ErrorTypeDefinition*));
    if (!ancestors) return NULL;
    
    current = type->parent;
    for (int i = 0; i < count && current; i++) {
        ancestors[i] = current;
        current = current->parent;
    }
    
    *ancestor_count = count;
    return ancestors;
}

// =============================================================================
// Type Lookup Functions
// =============================================================================

ErrorTypeDefinition* error_type_find_by_name(const char* name) {
    if (!name || !g_global_hierarchy) return NULL;
    
    pthread_mutex_lock(&g_hierarchy_mutex);
    g_stats.type_lookups++;
    
    for (int i = 0; i < g_global_hierarchy->type_count; i++) {
        if (strcmp(g_global_hierarchy->registered_types[i]->name, name) == 0) {
            ErrorTypeDefinition* result = g_global_hierarchy->registered_types[i];
            pthread_mutex_unlock(&g_hierarchy_mutex);
            return result;
        }
    }
    
    pthread_mutex_unlock(&g_hierarchy_mutex);
    return NULL;
}

ErrorTypeDefinition* error_type_find_by_id(uint64_t type_id) {
    if (!g_global_hierarchy) return NULL;
    
    pthread_mutex_lock(&g_hierarchy_mutex);
    g_stats.type_lookups++;
    
    for (int i = 0; i < g_global_hierarchy->type_lookup.lookup_count; i++) {
        if (g_global_hierarchy->type_lookup.type_ids[i] == type_id) {
            ErrorTypeDefinition* result = g_global_hierarchy->type_lookup.type_defs[i];
            pthread_mutex_unlock(&g_hierarchy_mutex);
            return result;
        }
    }
    
    pthread_mutex_unlock(&g_hierarchy_mutex);
    return NULL;
}

ErrorVariant* error_variant_find_by_name(ErrorTypeDefinition* type_def, const char* variant_name) {
    if (!type_def || !variant_name) return NULL;
    
    for (int i = 0; i < type_def->variant_count; i++) {
        if (strcmp(type_def->variants[i].name, variant_name) == 0) {
            return &type_def->variants[i];
        }
    }
    
    return NULL;
}

ErrorVariant* error_variant_find_by_code(ErrorTypeDefinition* type_def, int error_code) {
    if (!type_def) return NULL;
    
    for (int i = 0; i < type_def->variant_count; i++) {
        if (type_def->variants[i].error_code == error_code) {
            return &type_def->variants[i];
        }
    }
    
    return NULL;
}

// =============================================================================
// Structured Error Creation and Management
// =============================================================================

StructuredError* structured_error_new(ErrorTypeDefinition* type_def, 
                                      const char* variant_name) {
    if (!type_def || !variant_name) return NULL;
    
    ErrorVariant* variant = error_variant_find_by_name(type_def, variant_name);
    if (!variant) return NULL;
    
    return structured_error_new_with_variant(variant);
}

StructuredError* structured_error_new_with_variant(ErrorVariant* variant) {
    if (!variant) return NULL;
    
    StructuredError* error = calloc(1, sizeof(StructuredError));
    if (!error) return NULL;
    
    error->variant = variant;
    error->type_def = NULL; // Will be set based on variant
    error->field_count = variant->field_count;
    error->severity = variant->severity;
    error->timestamp_ms = get_current_time_ms();
    error->thread_id = get_thread_id();
    error->owns_field_memory = true;
    
    // Allocate field values array
    if (variant->field_count > 0) {
        error->field_values = calloc(variant->field_count, sizeof(ErrorFieldValue));
        if (!error->field_values) {
            free(error);
            return NULL;
        }
        
        // Initialize field values
        for (int i = 0; i < variant->field_count; i++) {
            ErrorFieldValue* field_value = &error->field_values[i];
            ErrorField* field_def = &variant->fields[i];
            
            field_value->field_name = duplicate_string(field_def->name);
            field_value->type = field_def->type;
            field_value->is_set = false;
            field_value->owns_memory = true;
        }
    }
    
    g_stats.errors_created++;
    return error;
}

void structured_error_free(StructuredError* error) {
    if (!error) return;
    
    // Free field values
    if (error->field_values && error->owns_field_memory) {
        for (int i = 0; i < error->field_count; i++) {
            ErrorFieldValue* field_value = &error->field_values[i];
            
            free((void*)field_value->field_name);
            
            if (field_value->owns_memory) {
                switch (field_value->type) {
                    case ERROR_FIELD_STRING:
                        free(field_value->string_value);
                        break;
                    case ERROR_FIELD_ARRAY:
                        free(field_value->array_value.data);
                        break;
                    case ERROR_FIELD_STRUCT:
                        // Recursively free nested structures
                        if (field_value->struct_value) {
                            // This would need a more sophisticated approach
                            // for nested structures
                        }
                        break;
                    default:
                        // Other types don't need special cleanup
                        break;
                }
            }
        }
        free(error->field_values);
    }
    
    // Free other allocated fields
    free((void*)error->message);
    free((void*)error->hint);
    free((void*)error->operation_context);
    
    // Free chained errors
    if (error->cause) {
        structured_error_free(error->cause);
    }
    
    // Free root cause
    if (error->root_cause) {
        goo_error_free(error->root_cause);
    }
    
    // Call custom destructor if set
    if (error->destructor) {
        error->destructor(error);
    }
    
    free(error);
}

// =============================================================================
// Field Value Setting Functions
// =============================================================================

static ErrorFieldValue* find_field_value(StructuredError* error, const char* field_name) {
    if (!error || !error->field_values || !field_name) return NULL;
    
    for (int i = 0; i < error->field_count; i++) {
        if (strcmp(error->field_values[i].field_name, field_name) == 0) {
            return &error->field_values[i];
        }
    }
    
    return NULL;
}

bool structured_error_set_string_field(StructuredError* error, const char* field_name, 
                                       const char* value) {
    ErrorFieldValue* field_value = find_field_value(error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_STRING) return false;
    
    // Free existing value if owned
    if (field_value->owns_memory && field_value->string_value) {
        free(field_value->string_value);
    }
    
    field_value->string_value = duplicate_string(value);
    field_value->is_set = true;
    field_value->owns_memory = true;
    
    return true;
}

bool structured_error_set_int_field(StructuredError* error, const char* field_name, 
                                    int64_t value) {
    ErrorFieldValue* field_value = find_field_value(error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_INT) return false;
    
    field_value->int_value = value;
    field_value->is_set = true;
    
    return true;
}

bool structured_error_set_float_field(StructuredError* error, const char* field_name, 
                                      double value) {
    ErrorFieldValue* field_value = find_field_value(error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_FLOAT) return false;
    
    field_value->float_value = value;
    field_value->is_set = true;
    
    return true;
}

bool structured_error_set_bool_field(StructuredError* error, const char* field_name, 
                                     bool value) {
    ErrorFieldValue* field_value = find_field_value(error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_BOOL) return false;
    
    field_value->bool_value = value;
    field_value->is_set = true;
    
    return true;
}

bool structured_error_set_enum_field(StructuredError* error, const char* field_name,
                                     const char* enum_value) {
    ErrorFieldValue* field_value = find_field_value(error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_ENUM) return false;
    
    // Find corresponding field definition to validate enum value
    ErrorField* field_def = NULL;
    if (error->variant) {
        for (int i = 0; i < error->variant->field_count; i++) {
            if (strcmp(error->variant->fields[i].name, field_name) == 0) {
                field_def = &error->variant->fields[i];
                break;
            }
        }
    }
    
    // Validate enum value
    if (field_def && field_def->enum_constraints.valid_values) {
        bool valid = false;
        for (int i = 0; i < field_def->enum_constraints.value_count; i++) {
            if (strcmp(field_def->enum_constraints.valid_values[i], enum_value) == 0) {
                valid = true;
                break;
            }
        }
        if (!valid) return false;
    }
    
    // Set the value
    if (field_value->owns_memory && field_value->string_value) {
        free(field_value->string_value);
    }
    
    field_value->string_value = duplicate_string(enum_value);
    field_value->is_set = true;
    field_value->owns_memory = true;
    
    return true;
}

// =============================================================================
// Field Value Getting Functions
// =============================================================================

bool structured_error_get_string_field(const StructuredError* error, const char* field_name,
                                       const char** value_out) {
    if (!value_out) return false;
    
    const ErrorFieldValue* field_value = find_field_value((StructuredError*)error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_STRING || !field_value->is_set) {
        return false;
    }
    
    *value_out = field_value->string_value;
    return true;
}

bool structured_error_get_int_field(const StructuredError* error, const char* field_name,
                                    int64_t* value_out) {
    if (!value_out) return false;
    
    const ErrorFieldValue* field_value = find_field_value((StructuredError*)error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_INT || !field_value->is_set) {
        return false;
    }
    
    *value_out = field_value->int_value;
    return true;
}

bool structured_error_get_float_field(const StructuredError* error, const char* field_name,
                                      double* value_out) {
    if (!value_out) return false;
    
    const ErrorFieldValue* field_value = find_field_value((StructuredError*)error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_FLOAT || !field_value->is_set) {
        return false;
    }
    
    *value_out = field_value->float_value;
    return true;
}

bool structured_error_get_bool_field(const StructuredError* error, const char* field_name,
                                     bool* value_out) {
    if (!value_out) return false;
    
    const ErrorFieldValue* field_value = find_field_value((StructuredError*)error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_BOOL || !field_value->is_set) {
        return false;
    }
    
    *value_out = field_value->bool_value;
    return true;
}

bool structured_error_get_enum_field(const StructuredError* error, const char* field_name,
                                     const char** value_out) {
    if (!value_out) return false;
    
    const ErrorFieldValue* field_value = find_field_value((StructuredError*)error, field_name);
    if (!field_value || field_value->type != ERROR_FIELD_ENUM || !field_value->is_set) {
        return false;
    }
    
    *value_out = field_value->string_value;
    return true;
}

// =============================================================================
// Error Validation and Message Generation
// =============================================================================

bool structured_error_validate(StructuredError* error) {
    if (!error || !error->variant) return false;
    
    // Check all required fields are set
    for (int i = 0; i < error->variant->field_count; i++) {
        ErrorField* field_def = &error->variant->fields[i];
        if (field_def->required) {
            ErrorFieldValue* field_value = find_field_value(error, field_def->name);
            if (!field_value || !field_value->is_set) {
                return false;
            }
        }
    }
    
    return true;
}

void structured_error_finalize(StructuredError* error) {
    if (!error) return;
    
    // Generate message and hint if not already set
    if (!error->message) {
        error->message = structured_error_generate_message(error);
    }
    
    if (!error->hint) {
        error->hint = structured_error_generate_hint(error);
    }
}

char* structured_error_generate_message(StructuredError* error) {
    if (!error || !error->variant) return duplicate_string("Unknown error");
    
    // Use format template if available
    if (error->variant->format_template) {
        // Simple template substitution (would need more sophisticated implementation)
        size_t template_len = strlen(error->variant->format_template);
        char* message = malloc(template_len + 256); // Extra space for substitutions
        if (message) {
            strcpy(message, error->variant->format_template);
            // TODO: Implement template variable substitution
            return message;
        }
    }
    
    // Fallback to variant description
    if (error->variant->description) {
        return duplicate_string(error->variant->description);
    }
    
    // Ultimate fallback
    char* message = malloc(256);
    if (message) {
        snprintf(message, 256, "Error in variant '%s'", 
                error->variant->name ? error->variant->name : "unknown");
    }
    
    return message;
}

char* structured_error_generate_hint(StructuredError* error) {
    if (!error || !error->variant) return NULL;
    
    // Use hint template if available
    if (error->variant->hint_template) {
        return duplicate_string(error->variant->hint_template);
    }
    
    // Generate hint from recovery actions
    if (error->variant->recovery_actions && error->variant->recovery_action_count > 0) {
        size_t total_len = 0;
        for (int i = 0; i < error->variant->recovery_action_count; i++) {
            total_len += strlen(error->variant->recovery_actions[i]) + 3; // "- " + "\n"
        }
        total_len += 50; // Header text
        
        char* hint = malloc(total_len);
        if (hint) {
            strcpy(hint, "Possible solutions:\n");
            for (int i = 0; i < error->variant->recovery_action_count; i++) {
                strcat(hint, "- ");
                strcat(hint, error->variant->recovery_actions[i]);
                strcat(hint, "\n");
            }
        }
        return hint;
    }
    
    return NULL;
}

char* structured_error_generate_detailed_info(StructuredError* error) {
    if (!error) return NULL;
    
    size_t buffer_size = 1024;
    char* info = malloc(buffer_size);
    if (!info) return NULL;
    
    snprintf(info, buffer_size,
        "Structured Error Details:\n"
        "  Variant: %s\n"
        "  Code: %d\n"
        "  Severity: %s\n"
        "  Timestamp: %lu ms\n"
        "  Thread ID: %lu\n",
        error->variant ? error->variant->name : "unknown",
        error->variant ? error->variant->error_code : 0,
        error_severity_to_string(error->severity),
        error->timestamp_ms,
        error->thread_id);
    
    // Add field information
    if (error->field_values && error->field_count > 0) {
        strcat(info, "  Fields:\n");
        for (int i = 0; i < error->field_count; i++) {
            ErrorFieldValue* field = &error->field_values[i];
            if (field->is_set) {
                char field_info[256];
                switch (field->type) {
                    case ERROR_FIELD_STRING:
                        snprintf(field_info, sizeof(field_info), 
                                "    %s: \"%s\"\n", field->field_name, field->string_value);
                        break;
                    case ERROR_FIELD_INT:
                        snprintf(field_info, sizeof(field_info),
                                "    %s: %ld\n", field->field_name, field->int_value);
                        break;
                    case ERROR_FIELD_FLOAT:
                        snprintf(field_info, sizeof(field_info),
                                "    %s: %f\n", field->field_name, field->float_value);
                        break;
                    case ERROR_FIELD_BOOL:
                        snprintf(field_info, sizeof(field_info),
                                "    %s: %s\n", field->field_name, 
                                field->bool_value ? "true" : "false");
                        break;
                    default:
                        snprintf(field_info, sizeof(field_info),
                                "    %s: [%s]\n", field->field_name,
                                error_field_type_to_string(field->type));
                        break;
                }
                strcat(info, field_info);
            }
        }
    }
    
    return info;
}

// =============================================================================
// Error Chaining Functions
// =============================================================================

void structured_error_set_cause(StructuredError* error, StructuredError* cause) {
    if (!error) return;
    
    // Free existing cause
    if (error->cause) {
        structured_error_free(error->cause);
    }
    
    error->cause = cause;
}

void structured_error_set_root_cause(StructuredError* error, goo_error_t* root_cause) {
    if (!error) return;
    
    // Free existing root cause
    if (error->root_cause) {
        goo_error_free(error->root_cause);
    }
    
    error->root_cause = root_cause;
}

// =============================================================================
// Pattern Matching Implementation
// =============================================================================

ErrorMatchPattern* error_match_pattern_new_type(ErrorTypeDefinition* type_def, bool include_subtypes) {
    if (!type_def) return NULL;
    
    ErrorMatchPattern* pattern = calloc(1, sizeof(ErrorMatchPattern));
    if (!pattern) return NULL;
    
    pattern->match_type = ERROR_MATCH_TYPE;
    pattern->type_match.type_def = type_def;
    pattern->type_match.include_subtypes = include_subtypes;
    
    return pattern;
}

ErrorMatchPattern* error_match_pattern_new_variant(ErrorVariant* variant) {
    if (!variant) return NULL;
    
    ErrorMatchPattern* pattern = calloc(1, sizeof(ErrorMatchPattern));
    if (!pattern) return NULL;
    
    pattern->match_type = ERROR_MATCH_VARIANT;
    pattern->variant_match.variant = variant;
    
    return pattern;
}

ErrorMatchPattern* error_match_pattern_new_field_value(const char* field_name, 
                                                      ErrorFieldType field_type,
                                                      const void* expected_value) {
    if (!field_name || !expected_value) return NULL;
    
    ErrorMatchPattern* pattern = calloc(1, sizeof(ErrorMatchPattern));
    if (!pattern) return NULL;
    
    pattern->match_type = ERROR_MATCH_FIELD_VALUE;
    pattern->field_match.field_name = duplicate_string(field_name);
    pattern->field_match.field_type = field_type;
    
    // Copy expected value based on type
    switch (field_type) {
        case ERROR_FIELD_STRING:
            pattern->field_match.expected_value.string_value = duplicate_string((const char*)expected_value);
            break;
        case ERROR_FIELD_INT:
            pattern->field_match.expected_value.int_value = *(const int64_t*)expected_value;
            break;
        case ERROR_FIELD_FLOAT:
            pattern->field_match.expected_value.float_value = *(const double*)expected_value;
            break;
        case ERROR_FIELD_BOOL:
            pattern->field_match.expected_value.bool_value = *(const bool*)expected_value;
            break;
        default:
            free(pattern);
            return NULL;
    }
    
    return pattern;
}

ErrorMatchPattern* error_match_pattern_new_severity(ErrorSeverity min_severity, 
                                                    ErrorSeverity max_severity) {
    ErrorMatchPattern* pattern = calloc(1, sizeof(ErrorMatchPattern));
    if (!pattern) return NULL;
    
    pattern->match_type = ERROR_MATCH_SEVERITY;
    pattern->severity_match.min_severity = min_severity;
    pattern->severity_match.max_severity = max_severity;
    
    return pattern;
}

ErrorMatchPattern* error_match_pattern_new_combination(ErrorMatchPattern** patterns,
                                                      int pattern_count, bool match_all) {
    if (!patterns || pattern_count <= 0) return NULL;
    
    ErrorMatchPattern* pattern = calloc(1, sizeof(ErrorMatchPattern));
    if (!pattern) return NULL;
    
    pattern->match_type = ERROR_MATCH_COMBINATION;
    pattern->combination_match.sub_patterns = malloc(pattern_count * sizeof(ErrorMatchPattern*));
    if (!pattern->combination_match.sub_patterns) {
        free(pattern);
        return NULL;
    }
    
    // Copy pattern pointers
    for (int i = 0; i < pattern_count; i++) {
        pattern->combination_match.sub_patterns[i] = patterns[i];
    }
    
    pattern->combination_match.pattern_count = pattern_count;
    pattern->combination_match.match_all = match_all;
    
    return pattern;
}

void error_match_pattern_free(ErrorMatchPattern* pattern) {
    if (!pattern) return;
    
    switch (pattern->match_type) {
        case ERROR_MATCH_FIELD_VALUE:
            free((void*)pattern->field_match.field_name);
            if (pattern->field_match.field_type == ERROR_FIELD_STRING) {
                free((void*)pattern->field_match.expected_value.string_value);
            }
            break;
            
        case ERROR_MATCH_FIELD_PATTERN:
            free((void*)pattern->pattern_match.field_name);
            free((void*)pattern->pattern_match.regex_pattern);
            break;
            
        case ERROR_MATCH_COMBINATION:
            // Note: We don't free sub_patterns themselves as they might be shared
            free(pattern->combination_match.sub_patterns);
            break;
            
        default:
            // Other patterns don't have dynamically allocated fields
            break;
    }
    
    free(pattern);
}

bool structured_error_matches_pattern(const StructuredError* error, 
                                      const ErrorMatchPattern* pattern) {
    if (!error || !pattern) return false;
    
    pthread_mutex_lock(&g_hierarchy_mutex);
    g_stats.pattern_matches_performed++;
    pthread_mutex_unlock(&g_hierarchy_mutex);
    
    switch (pattern->match_type) {
        case ERROR_MATCH_TYPE: {
            if (!error->type_def) return false;
            
            if (error->type_def == pattern->type_match.type_def) {
                return true;
            }
            
            // Check inheritance if include_subtypes is true
            if (pattern->type_match.include_subtypes) {
                return error_type_is_subtype_of(error->type_def, pattern->type_match.type_def);
            }
            
            return false;
        }
        
        case ERROR_MATCH_VARIANT: {
            return error->variant == pattern->variant_match.variant;
        }
        
        case ERROR_MATCH_FIELD_VALUE: {
            const ErrorFieldValue* field_value = find_field_value((StructuredError*)error, 
                                                                 pattern->field_match.field_name);
            if (!field_value || !field_value->is_set || 
                field_value->type != pattern->field_match.field_type) {
                return false;
            }
            
            // Compare values based on type
            switch (pattern->field_match.field_type) {
                case ERROR_FIELD_STRING:
                    return strcmp(field_value->string_value, 
                                pattern->field_match.expected_value.string_value) == 0;
                case ERROR_FIELD_INT:
                    return field_value->int_value == pattern->field_match.expected_value.int_value;
                case ERROR_FIELD_FLOAT:
                    return fabs(field_value->float_value - pattern->field_match.expected_value.float_value) < 0.001;
                case ERROR_FIELD_BOOL:
                    return field_value->bool_value == pattern->field_match.expected_value.bool_value;
                default:
                    return false;
            }
        }
        
        case ERROR_MATCH_SEVERITY: {
            return error->severity >= pattern->severity_match.min_severity &&
                   error->severity <= pattern->severity_match.max_severity;
        }
        
        case ERROR_MATCH_COMBINATION: {
            bool result = pattern->combination_match.match_all; // Start with true for AND, false for OR
            
            for (int i = 0; i < pattern->combination_match.pattern_count; i++) {
                bool sub_match = structured_error_matches_pattern(error, 
                                                                pattern->combination_match.sub_patterns[i]);
                
                if (pattern->combination_match.match_all) {
                    // AND logic: all must match
                    result = result && sub_match;
                    if (!result) break; // Short-circuit
                } else {
                    // OR logic: any must match
                    result = result || sub_match;
                    if (result) break; // Short-circuit
                }
            }
            
            return result;
        }
        
        default:
            return false;
    }
}

StructuredError** structured_error_filter_by_pattern(StructuredError** errors, int error_count,
                                                     const ErrorMatchPattern* pattern,
                                                     int* matched_count) {
    if (!errors || error_count <= 0 || !pattern || !matched_count) return NULL;
    
    *matched_count = 0;
    
    // First pass: count matches
    for (int i = 0; i < error_count; i++) {
        if (structured_error_matches_pattern(errors[i], pattern)) {
            (*matched_count)++;
        }
    }
    
    if (*matched_count == 0) return NULL;
    
    // Second pass: collect matches
    StructuredError** matched_errors = malloc(*matched_count * sizeof(StructuredError*));
    if (!matched_errors) {
        *matched_count = 0;
        return NULL;
    }
    
    int match_index = 0;
    for (int i = 0; i < error_count; i++) {
        if (structured_error_matches_pattern(errors[i], pattern)) {
            matched_errors[match_index++] = errors[i];
        }
    }
    
    return matched_errors;
}

// =============================================================================
// Error Dispatcher Implementation
// =============================================================================

ErrorDispatcher* error_dispatcher_new(void) {
    ErrorDispatcher* dispatcher = calloc(1, sizeof(ErrorDispatcher));
    if (!dispatcher) return NULL;
    
    dispatcher->entries = NULL;
    dispatcher->entry_count = 0;
    dispatcher->entry_capacity = 0;
    dispatcher->default_handler = NULL;
    dispatcher->default_context = NULL;
    
    memset(&dispatcher->stats, 0, sizeof(dispatcher->stats));
    
    return dispatcher;
}

void error_dispatcher_free(ErrorDispatcher* dispatcher) {
    if (!dispatcher) return;
    
    // Free dispatch entries
    for (int i = 0; i < dispatcher->entry_count; i++) {
        ErrorDispatchEntry* entry = &dispatcher->entries[i];
        
        // Note: We don't free the pattern as it might be shared
        // The caller is responsible for managing pattern lifetime
        free((void*)entry->handler_name);
    }
    
    free(dispatcher->entries);
    free(dispatcher);
}

void error_dispatcher_register_handler(ErrorDispatcher* dispatcher,
                                       ErrorMatchPattern* pattern,
                                       StructuredErrorHandler handler,
                                       void* context,
                                       int priority,
                                       const char* handler_name) {
    if (!dispatcher || !pattern || !handler) return;
    
    // Expand entries array if needed
    if (dispatcher->entry_count >= dispatcher->entry_capacity) {
        int new_capacity = dispatcher->entry_capacity == 0 ? 8 : dispatcher->entry_capacity * 2;
        ErrorDispatchEntry* new_entries = realloc(dispatcher->entries,
                                                 new_capacity * sizeof(ErrorDispatchEntry));
        if (!new_entries) return;
        
        dispatcher->entries = new_entries;
        dispatcher->entry_capacity = new_capacity;
    }
    
    // Insert entry in priority order (higher priority first)
    int insert_index = dispatcher->entry_count;
    for (int i = 0; i < dispatcher->entry_count; i++) {
        if (priority > dispatcher->entries[i].priority) {
            insert_index = i;
            break;
        }
    }
    
    // Shift existing entries if needed
    if (insert_index < dispatcher->entry_count) {
        memmove(&dispatcher->entries[insert_index + 1],
                &dispatcher->entries[insert_index],
                (dispatcher->entry_count - insert_index) * sizeof(ErrorDispatchEntry));
    }
    
    // Initialize new entry
    ErrorDispatchEntry* entry = &dispatcher->entries[insert_index];
    entry->pattern = pattern;
    entry->handler = handler;
    entry->handler_context = context;
    entry->priority = priority;
    entry->handler_name = duplicate_string(handler_name);
    
    dispatcher->entry_count++;
}

void error_dispatcher_set_default_handler(ErrorDispatcher* dispatcher,
                                          StructuredErrorHandler handler,
                                          void* context) {
    if (!dispatcher) return;
    
    dispatcher->default_handler = handler;
    dispatcher->default_context = context;
}

void error_dispatcher_dispatch(ErrorDispatcher* dispatcher, const StructuredError* error) {
    if (!dispatcher || !error) return;
    
    dispatcher->stats.dispatches_performed++;
    
    // Try each registered handler in priority order
    for (int i = 0; i < dispatcher->entry_count; i++) {
        ErrorDispatchEntry* entry = &dispatcher->entries[i];
        
        if (structured_error_matches_pattern(error, entry->pattern)) {
            dispatcher->stats.patterns_matched++;
            entry->handler(error, entry->handler_context);
            return; // Handler found and executed
        }
    }
    
    // No handler matched, use default handler
    if (dispatcher->default_handler) {
        dispatcher->stats.default_handler_calls++;
        dispatcher->default_handler(error, dispatcher->default_context);
    }
}

int error_dispatcher_dispatch_batch(ErrorDispatcher* dispatcher, 
                                    StructuredError** errors, int error_count) {
    if (!dispatcher || !errors || error_count <= 0) return 0;
    
    int handled_count = 0;
    
    for (int i = 0; i < error_count; i++) {
        if (errors[i]) {
            error_dispatcher_dispatch(dispatcher, errors[i]);
            handled_count++;
        }
    }
    
    return handled_count;
}

// =============================================================================
// Integration with Existing Error System
// =============================================================================

goo_error_t* structured_error_to_runtime_error(const StructuredError* structured_error) {
    if (!structured_error) return NULL;
    
    // Generate message if not already set
    char* message = structured_error->message ? 
                   duplicate_string(structured_error->message) :
                   structured_error_generate_message((StructuredError*)structured_error);
    
    if (!message) {
        message = duplicate_string("Structured error");
    }
    
    // Determine error code
    int error_code = structured_error->variant ? 
                    structured_error->variant->error_code : 0;
    
    goo_error_t* runtime_error = goo_new_error_with_code(message, error_code);
    free(message);
    
    // Chain root cause if available
    if (structured_error->root_cause && runtime_error) {
        runtime_error->cause = goo_new_error_with_code(
            structured_error->root_cause->message,
            structured_error->root_cause->code
        );
    }
    
    return runtime_error;
}

StructuredError* runtime_error_to_structured_error(const goo_error_t* runtime_error,
                                                   ErrorTypeDefinition* target_type) {
    if (!runtime_error || !target_type) return NULL;
    
    // Find variant by error code
    ErrorVariant* variant = error_variant_find_by_code(target_type, runtime_error->code);
    if (!variant) {
        // Use first variant as fallback
        variant = target_type->variant_count > 0 ? &target_type->variants[0] : NULL;
    }
    
    if (!variant) return NULL;
    
    StructuredError* structured_error = structured_error_new_with_variant(variant);
    if (!structured_error) return NULL;
    
    // Set message from runtime error
    structured_error->message = duplicate_string(runtime_error->message);
    
    // Set root cause
    if (runtime_error->cause) {
        structured_error->root_cause = goo_new_error_with_code(
            runtime_error->cause->message,
            runtime_error->cause->code
        );
    }
    
    return structured_error;
}

goo_error_union_t* structured_error_to_error_union(StructuredError* structured_error) {
    if (!structured_error) return NULL;
    
    goo_error_t* runtime_error = structured_error_to_runtime_error(structured_error);
    if (!runtime_error) return NULL;
    
    return goo_error_union_new_error(runtime_error);
}

StructuredError* error_union_to_structured_error(goo_error_union_t* error_union,
                                                 ErrorTypeDefinition* target_type) {
    if (!error_union || !goo_error_union_is_error(error_union) || !target_type) return NULL;
    
    goo_error_t* runtime_error = goo_error_union_get_error(error_union);
    if (!runtime_error) return NULL;
    
    return runtime_error_to_structured_error(runtime_error, target_type);
}

// =============================================================================
// Configuration and Statistics
// =============================================================================

void configure_error_hierarchy_system(const ErrorHierarchyConfig* config) {
    if (!config) return;
    
    pthread_mutex_lock(&g_hierarchy_mutex);
    g_config = *config;
    pthread_mutex_unlock(&g_hierarchy_mutex);
}

ErrorHierarchyConfig* get_error_hierarchy_config(void) {
    return &g_config;
}

ErrorHierarchyStats get_error_hierarchy_stats(void) {
    pthread_mutex_lock(&g_hierarchy_mutex);
    ErrorHierarchyStats stats = g_stats;
    pthread_mutex_unlock(&g_hierarchy_mutex);
    return stats;
}

void print_error_hierarchy_performance_report(void) {
    ErrorHierarchyStats stats = get_error_hierarchy_stats();
    
    printf("🔄 Error Hierarchy Performance Report\n");
    printf("════════════════════════════════════\n");
    printf("Types and Variants:\n");
    printf("  Types Registered:         %lu\n", stats.types_registered);
    printf("  Variants Defined:         %lu\n", stats.variants_defined);
    printf("  Errors Created:           %lu\n", stats.errors_created);
    printf("\nOperations:\n");
    printf("  Pattern Matches:          %lu\n", stats.pattern_matches_performed);
    printf("  Inheritance Queries:      %lu\n", stats.inheritance_queries);
    printf("  Type Lookups:             %lu\n", stats.type_lookups);
    printf("\nMemory Usage:\n");
    printf("  Current Memory:           %lu bytes\n", stats.memory_used_bytes);
    printf("  Peak Memory:              %lu bytes\n", stats.peak_memory_bytes);
    printf("\nPerformance:\n");
    printf("  Avg Lookup Time:          %.3f ms\n", stats.average_lookup_time_ms);
    printf("  Avg Match Time:           %.3f ms\n", stats.average_match_time_ms);
    printf("════════════════════════════════════\n");
}

// =============================================================================
// Utility Functions
// =============================================================================

const char* error_field_type_to_string(ErrorFieldType type) {
    switch (type) {
        case ERROR_FIELD_STRING: return "string";
        case ERROR_FIELD_INT: return "int";
        case ERROR_FIELD_FLOAT: return "float";
        case ERROR_FIELD_BOOL: return "bool";
        case ERROR_FIELD_ENUM: return "enum";
        case ERROR_FIELD_STRUCT: return "struct";
        case ERROR_FIELD_ARRAY: return "array";
        case ERROR_FIELD_OPTIONAL: return "optional";
        default: return "unknown";
    }
}

const char* error_match_type_to_string(ErrorMatchType type) {
    switch (type) {
        case ERROR_MATCH_TYPE: return "type";
        case ERROR_MATCH_VARIANT: return "variant";
        case ERROR_MATCH_FIELD_VALUE: return "field_value";
        case ERROR_MATCH_FIELD_RANGE: return "field_range";
        case ERROR_MATCH_FIELD_PATTERN: return "field_pattern";
        case ERROR_MATCH_INHERITANCE: return "inheritance";
        case ERROR_MATCH_SEVERITY: return "severity";
        case ERROR_MATCH_COMBINATION: return "combination";
        default: return "unknown";
    }
}

void print_error_type_definition(const ErrorTypeDefinition* type_def) {
    if (!type_def) return;
    
    printf("Error Type: %s\n", type_def->name);
    printf("  Description: %s\n", type_def->description);
    printf("  Type ID: %lu\n", type_def->type_id);
    printf("  Variants: %d\n", type_def->variant_count);
    
    for (int i = 0; i < type_def->variant_count; i++) {
        ErrorVariant* variant = &type_def->variants[i];
        printf("    %s (code: %d)\n", variant->name, variant->error_code);
        printf("      %s\n", variant->description);
        printf("      Fields: %d\n", variant->field_count);
        
        for (int j = 0; j < variant->field_count; j++) {
            ErrorField* field = &variant->fields[j];
            printf("        %s: %s%s\n", field->name,
                   error_field_type_to_string(field->type),
                   field->required ? " (required)" : "");
        }
    }
    
    if (type_def->parent) {
        printf("  Parent: %s\n", type_def->parent->name);
    }
    
    if (type_def->child_count > 0) {
        printf("  Children: ");
        for (int i = 0; i < type_def->child_count; i++) {
            printf("%s", type_def->children[i]->name);
            if (i < type_def->child_count - 1) printf(", ");
        }
        printf("\n");
    }
}

void print_structured_error(const StructuredError* error) {
    if (!error) return;
    
    printf("Structured Error:\n");
    if (error->variant) {
        printf("  Variant: %s\n", error->variant->name);
    }
    if (error->message) {
        printf("  Message: %s\n", error->message);
    }
    if (error->hint) {
        printf("  Hint: %s\n", error->hint);
    }
    
    printf("  Severity: %s\n", error_severity_to_string(error->severity));
    printf("  Timestamp: %lu ms\n", error->timestamp_ms);
    
    if (error->field_values && error->field_count > 0) {
        printf("  Fields:\n");
        for (int i = 0; i < error->field_count; i++) {
            ErrorFieldValue* field = &error->field_values[i];
            if (field->is_set) {
                printf("    %s = ", field->field_name);
                switch (field->type) {
                    case ERROR_FIELD_STRING:
                        printf("\"%s\"\n", field->string_value);
                        break;
                    case ERROR_FIELD_INT:
                        printf("%ld\n", field->int_value);
                        break;
                    case ERROR_FIELD_FLOAT:
                        printf("%f\n", field->float_value);
                        break;
                    case ERROR_FIELD_BOOL:
                        printf("%s\n", field->bool_value ? "true" : "false");
                        break;
                    default:
                        printf("[%s]\n", error_field_type_to_string(field->type));
                        break;
                }
            }
        }
    }
    
    if (error->cause) {
        printf("  Caused by:\n");
        print_structured_error(error->cause);
    }
}

void print_error_hierarchy_stats(const ErrorHierarchy* hierarchy) {
    if (!hierarchy) return;
    
    printf("Error Hierarchy Statistics:\n");
    printf("  Registered Types: %d\n", hierarchy->type_count);
    printf("  Type Capacity: %d\n", hierarchy->type_capacity);
    printf("  Inheritance Relationships: %d\n", hierarchy->inheritance_count);
    printf("  Statistics:\n");
    printf("    Errors Created: %lu\n", hierarchy->stats.errors_created);
    printf("    Errors Matched: %lu\n", hierarchy->stats.errors_matched);
    printf("    Type Checks: %lu\n", hierarchy->stats.type_checks_performed);
    printf("    Inheritance Queries: %lu\n", hierarchy->stats.inheritance_queries);
}