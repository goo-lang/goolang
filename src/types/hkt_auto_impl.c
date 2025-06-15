#include "interface_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Automatic Implementation Generation for Built-in Types
// =============================================================================

// Structure to hold auto-generated implementation info
typedef struct AutoImplInfo {
    char* type_name;              // Name of the type (e.g., "Vec", "Option")
    char* concept_name;           // Name of the concept (e.g., "Functor", "Monad")
    InterfaceMethod* methods;     // Generated methods
    struct AutoImplInfo* next;    // For linked list
} AutoImplInfo;

// Global registry of auto-generated implementations
static AutoImplInfo* g_auto_impls = NULL;

// Helper function for string duplication
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// =============================================================================
// Functor Implementation Generation
// =============================================================================

// Generate map method for Vec type
static InterfaceMethod* generate_vec_map_method(void) {
    InterfaceMethod* method = malloc(sizeof(InterfaceMethod));
    if (!method) return NULL;
    
    method->name = str_dup("map");
    
    // Create function type: fn<A, B>(self: Vec<A>, f: fn(A) -> B) -> Vec<B>
    Type* a_param = type_param("A", 0, NULL);
    Type* b_param = type_param("B", 1, NULL);
    
    // Create Vec<A> type
    Type* vec_constructor = type_constructor("Vec", 1, "* -> *");
    Type* vec_a_args[] = { a_param };
    Type* vec_a = type_application(vec_constructor, vec_a_args, 1);
    
    // Create fn(A) -> B type
    Type* map_func_params[] = { a_param };
    Type* map_func_type = type_function(map_func_params, 1, b_param);
    
    // Create Vec<B> return type
    Type* vec_b_args[] = { b_param };
    Type* vec_b = type_application(vec_constructor, vec_b_args, 1);
    
    // Create method signature
    Type* method_params[] = { vec_a, map_func_type };
    method->type = type_function(method_params, 2, vec_b);
    method->next = NULL;
    
    return method;
}

// Generate map method for Option type
static InterfaceMethod* generate_option_map_method(void) {
    InterfaceMethod* method = malloc(sizeof(InterfaceMethod));
    if (!method) return NULL;
    
    method->name = str_dup("map");
    
    // Create function type: fn<A, B>(self: Option<A>, f: fn(A) -> B) -> Option<B>
    Type* a_param = type_param("A", 0, NULL);
    Type* b_param = type_param("B", 1, NULL);
    
    // Create Option<A> type
    Type* option_constructor = type_constructor("Option", 1, "* -> *");
    Type* option_a_args[] = { a_param };
    Type* option_a = type_application(option_constructor, option_a_args, 1);
    
    // Create fn(A) -> B type
    Type* map_func_params[] = { a_param };
    Type* map_func_type = type_function(map_func_params, 1, b_param);
    
    // Create Option<B> return type
    Type* option_b_args[] = { b_param };
    Type* option_b = type_application(option_constructor, option_b_args, 1);
    
    // Create method signature
    Type* method_params[] = { option_a, map_func_type };
    method->type = type_function(method_params, 2, option_b);
    method->next = NULL;
    
    return method;
}

// Generate map method for Result type
static InterfaceMethod* generate_result_map_method(void) {
    InterfaceMethod* method = malloc(sizeof(InterfaceMethod));
    if (!method) return NULL;
    
    method->name = str_dup("map");
    
    // Create function type: fn<A, B, E>(self: Result<A, E>, f: fn(A) -> B) -> Result<B, E>
    Type* a_param = type_param("A", 0, NULL);
    Type* b_param = type_param("B", 1, NULL);
    Type* e_param = type_param("E", 2, NULL);
    
    // Create Result<A, E> type
    Type* result_constructor = type_constructor("Result", 2, "* -> * -> *");
    Type* result_a_args[] = { a_param, e_param };
    Type* result_a = type_application(result_constructor, result_a_args, 2);
    
    // Create fn(A) -> B type
    Type* map_func_params[] = { a_param };
    Type* map_func_type = type_function(map_func_params, 1, b_param);
    
    // Create Result<B, E> return type
    Type* result_b_args[] = { b_param, e_param };
    Type* result_b = type_application(result_constructor, result_b_args, 2);
    
    // Create method signature
    Type* method_params[] = { result_a, map_func_type };
    method->type = type_function(method_params, 2, result_b);
    method->next = NULL;
    
    return method;
}

// =============================================================================
// Monad Implementation Generation
// =============================================================================

// Generate pure/return method for Option
static InterfaceMethod* generate_option_pure_method(void) {
    InterfaceMethod* method = malloc(sizeof(InterfaceMethod));
    if (!method) return NULL;
    
    method->name = str_dup("pure");
    
    // Create function type: fn<T>(value: T) -> Option<T>
    Type* t_param = type_param("T", 0, NULL);
    
    // Create Option<T> return type
    Type* option_constructor = type_constructor("Option", 1, "* -> *");
    Type* option_t_args[] = { t_param };
    Type* option_t = type_application(option_constructor, option_t_args, 1);
    
    // Create method signature
    Type* method_params[] = { t_param };
    method->type = type_function(method_params, 1, option_t);
    method->next = NULL;
    
    return method;
}

// Generate bind method for Option
static InterfaceMethod* generate_option_bind_method(void) {
    InterfaceMethod* method = malloc(sizeof(InterfaceMethod));
    if (!method) return NULL;
    
    method->name = str_dup("bind");
    
    // Create function type: fn<A, B>(self: Option<A>, f: fn(A) -> Option<B>) -> Option<B>
    Type* a_param = type_param("A", 0, NULL);
    Type* b_param = type_param("B", 1, NULL);
    
    // Create Option<A> type
    Type* option_constructor = type_constructor("Option", 1, "* -> *");
    Type* option_a_args[] = { a_param };
    Type* option_a = type_application(option_constructor, option_a_args, 1);
    
    // Create Option<B> type
    Type* option_b_args[] = { b_param };
    Type* option_b = type_application(option_constructor, option_b_args, 1);
    
    // Create fn(A) -> Option<B> type
    Type* bind_func_params[] = { a_param };
    Type* bind_func_type = type_function(bind_func_params, 1, option_b);
    
    // Create method signature
    Type* method_params[] = { option_a, bind_func_type };
    method->type = type_function(method_params, 2, option_b);
    method->next = NULL;
    
    return method;
}

// =============================================================================
// Registration and Lookup Functions
// =============================================================================

// Register an auto-generated implementation
static void register_auto_impl(const char* type_name, const char* concept_name, 
                              InterfaceMethod* methods) {
    AutoImplInfo* impl = malloc(sizeof(AutoImplInfo));
    if (!impl) return;
    
    impl->type_name = str_dup(type_name);
    impl->concept_name = str_dup(concept_name);
    impl->methods = methods;
    impl->next = g_auto_impls;
    g_auto_impls = impl;
}

// Find auto-generated implementation
static AutoImplInfo* find_auto_impl(const char* type_name, const char* concept_name) {
    for (AutoImplInfo* impl = g_auto_impls; impl; impl = impl->next) {
        if (strcmp(impl->type_name, type_name) == 0 &&
            strcmp(impl->concept_name, concept_name) == 0) {
            return impl;
        }
    }
    return NULL;
}

// =============================================================================
// Main Auto-Implementation Generation Function
// =============================================================================

int generate_builtin_hkt_implementations(void) {
    // Generate Functor implementations
    {
        // Vec implements Functor
        InterfaceMethod* vec_map = generate_vec_map_method();
        register_auto_impl("Vec", "Functor", vec_map);
        
        // Option implements Functor
        InterfaceMethod* option_map = generate_option_map_method();
        register_auto_impl("Option", "Functor", option_map);
        
        // Result implements Functor
        InterfaceMethod* result_map = generate_result_map_method();
        register_auto_impl("Result", "Functor", result_map);
    }
    
    // Generate Monad implementations
    {
        // Option implements Monad
        InterfaceMethod* option_pure = generate_option_pure_method();
        InterfaceMethod* option_bind = generate_option_bind_method();
        option_pure->next = option_bind;
        register_auto_impl("Option", "Monad", option_pure);
    }
    
    return 1; // Success
}

// Check if a type has an auto-generated implementation for a concept
int has_auto_impl(const char* type_name, const char* concept_name) {
    return find_auto_impl(type_name, concept_name) != NULL;
}

// Get the auto-generated methods for a type and concept
InterfaceMethod* get_auto_impl_methods(const char* type_name, const char* concept_name) {
    AutoImplInfo* impl = find_auto_impl(type_name, concept_name);
    return impl ? impl->methods : NULL;
}

// =============================================================================
// Cleanup Function
// =============================================================================

void cleanup_auto_impls(void) {
    AutoImplInfo* current = g_auto_impls;
    while (current) {
        AutoImplInfo* next = current->next;
        free(current->type_name);
        free(current->concept_name);
        
        // Free methods
        InterfaceMethod* method = current->methods;
        while (method) {
            InterfaceMethod* next_method = method->next;
            free(method->name);
            type_free(method->type);
            free(method);
            method = next_method;
        }
        
        free(current);
        current = next;
    }
    g_auto_impls = NULL;
}