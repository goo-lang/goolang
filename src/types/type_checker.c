#include "types/constraint_inference.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Type checker initialization and cleanup

TypeChecker* type_checker_new(void) {
    TypeChecker* checker = malloc(sizeof(TypeChecker));
    if (!checker) return NULL;

    checker->current_scope = scope_new(NULL);
    checker->next_scope_id = 1;
    checker->builtin_types = NULL;
    checker->current_file = NULL;
    checker->error_count = 0;
    checker->warning_count = 0;
    checker->current_function_return_type = NULL;
    checker->type_cache = NULL;
    checker->type_cache_size = 0;
    checker->type_cache_capacity = 0;

    type_checker_init_builtins(checker);

    return checker;
}

void type_checker_free(TypeChecker* checker) {
    if (!checker) return;
    
    scope_free(checker->current_scope);
    
    // Free builtin types
    if (checker->builtin_types) {
        for (int i = 0; i < TYPE_COUNT; i++) {
            if (checker->builtin_types[i]) {
                type_free(checker->builtin_types[i]);
            }
        }
        free(checker->builtin_types);
    }
    
    // Free type cache
    if (checker->type_cache) {
        for (size_t i = 0; i < checker->type_cache_size; i++) {
            if (checker->type_cache[i]) {
                type_free(checker->type_cache[i]);
            }
        }
        free(checker->type_cache);
    }
    
    free(checker->current_file);
    free(checker);
}

void type_checker_init_builtins(TypeChecker* checker) {
    if (!checker) return;
    
    checker->builtin_types = malloc(sizeof(Type*) * TYPE_COUNT);
    if (!checker->builtin_types) return;
    
    memset(checker->builtin_types, 0, sizeof(Type*) * TYPE_COUNT);
    
    // Initialize builtin types
    checker->builtin_types[TYPE_VOID] = type_void();
    checker->builtin_types[TYPE_BOOL] = type_bool();
    checker->builtin_types[TYPE_INT8] = type_int(8, 1);
    checker->builtin_types[TYPE_INT16] = type_int(16, 1);
    checker->builtin_types[TYPE_INT32] = type_int(32, 1);
    checker->builtin_types[TYPE_INT64] = type_int(64, 1);
    checker->builtin_types[TYPE_UINT8] = type_int(8, 0);
    checker->builtin_types[TYPE_UINT16] = type_int(16, 0);
    checker->builtin_types[TYPE_UINT32] = type_int(32, 0);
    checker->builtin_types[TYPE_UINT64] = type_int(64, 0);
    checker->builtin_types[TYPE_FLOAT32] = type_float(32);
    checker->builtin_types[TYPE_FLOAT64] = type_float(64);
    checker->builtin_types[TYPE_STRING] = type_string_type();
    checker->builtin_types[TYPE_CHAR] = type_char();
    
    // Add built-in functions to the global scope
    type_checker_add_builtin_functions(checker);
}

Type* type_checker_get_builtin(TypeChecker* checker, TypeKind kind) {
    if (!checker || !checker->builtin_types || kind >= TYPE_COUNT) {
        return NULL;
    }
    return checker->builtin_types[kind];
}

void type_checker_add_builtin_functions(TypeChecker* checker) {
    if (!checker || !checker->current_scope) return;
    
    // make_chan(type, capacity) -> chan type
    // For now, treat make_chan as a generic function - we'll handle it specially in expression checking
    Type* make_chan_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    Variable* make_chan_var = variable_new("make_chan", make_chan_type, (Position){0, 0, 0, "builtin"});
    if (make_chan_var) {
        make_chan_var->is_builtin = 1;
        make_chan_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, make_chan_var);
    }
    
    // println(args...) -> void
    Type* println_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]); // variadic
    Variable* println_var = variable_new("println", println_type, (Position){0, 0, 0, "builtin"});
    if (println_var) {
        println_var->is_builtin = 1;
        println_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, println_var);
    }
    
    // print(args...) -> void
    Type* print_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]); // variadic
    Variable* print_var = variable_new("print", print_type, (Position){0, 0, 0, "builtin"});
    if (print_var) {
        print_var->is_builtin = 1;
        print_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, print_var);
    }
    
    // goo_printf(format, args...) -> void
    Type* goo_printf_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]); // variadic
    Variable* goo_printf_var = variable_new("goo_printf", goo_printf_type, (Position){0, 0, 0, "builtin"});
    if (goo_printf_var) {
        goo_printf_var->is_builtin = 1;
        goo_printf_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, goo_printf_var);
    }

    // error(message string) -> error
    // Used in error union returns: return error("something went wrong")
    // Create special "error" type that's compatible with any error union
    Type* error_return_type = type_new(TYPE_UNKNOWN);
    if (error_return_type) {
        error_return_type->name = strdup("error");
    }
    Type* error_func_type = type_function(NULL, 0, error_return_type);
    Variable* error_var = variable_new("error", error_func_type, (Position){0, 0, 0, "builtin"});
    if (error_var) {
        error_var->is_builtin = 1;
        error_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, error_var);
    }
}

void type_checker_add_fmt_functions(TypeChecker* checker) {
    if (!checker || !checker->current_scope) return;
    
    // Add fmt as a module/package variable
    Type* fmt_type = type_new(TYPE_INTERFACE); // Treat fmt as an interface for now
    Variable* fmt_var = variable_new("fmt", fmt_type, (Position){0, 0, 0, "builtin"});
    if (fmt_var) {
        fmt_var->is_builtin = 1;
        fmt_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, fmt_var);
    }
}

// Scope management

void scope_push(TypeChecker* checker) {
    if (!checker) return;
    
    Scope* new_scope = scope_new(checker->current_scope);
    if (new_scope) {
        new_scope->scope_id = checker->next_scope_id++;
        checker->current_scope = new_scope;
    }
}

void scope_pop(TypeChecker* checker) {
    if (!checker || !checker->current_scope) return;
    
    Scope* old_scope = checker->current_scope;
    checker->current_scope = old_scope->parent;
    scope_free(old_scope);
}

Variable* type_checker_lookup_variable(TypeChecker* checker, const char* name) {
    if (!checker || !checker->current_scope) return NULL;
    return scope_lookup_variable(checker->current_scope, name);
}

// Type checking entry points

int type_check_program(TypeChecker* checker, ASTNode* program) {
    if (!checker || !program) return 0;
    
    if (program->type != AST_PROGRAM) {
        type_error(checker, program->pos, "Expected program node");
        return 0;
    }
    
    ProgramNode* prog = (ProgramNode*)program;
    
    // Type check imports
    if (prog->imports) {
        ASTNode* import = prog->imports;
        while (import) {
            // Basic import handling - for now just add fmt package
            if (import->type == AST_IMPORT_SPEC) {
                // TODO: Get actual import path and handle different packages
                // For now, assume fmt import and add fmt functions
                type_checker_add_fmt_functions(checker);
            }
            import = import->next;
        }
    }
    
    // Two-pass type checking:
    // Pass 1: Register all type declarations first
    if (prog->decls) {
        ASTNode* decl = prog->decls;
        while (decl) {
            if (decl->type == AST_TYPE_DECL) {
                if (!type_check_declaration(checker, decl)) {
                    return 0;
                }
            }
            decl = decl->next;
        }
    }

    // Pass 2: Type check all other declarations
    if (prog->decls) {
        ASTNode* decl = prog->decls;
        while (decl) {
            if (decl->type != AST_TYPE_DECL) {
                if (!type_check_declaration(checker, decl)) {
                    return 0;
                }
            }
            decl = decl->next;
        }
    }
    
    // Perform ownership and memory safety analysis with escape analysis
    // TODO: Re-enable when flow analysis integration is fixed
    // if (checker->error_count == 0) {
    //     printf("🔍 Ownership tracking system initialized\n");
    //     printf("✅ Memory management: ownership-based (no GC)\n");
    //     printf("📊 Runtime supports: stack/heap/arena allocation, reference counting\n");
    //
    //     // Now enabled: full ownership analysis with escape analysis integration
    //     if (perform_ownership_analysis(checker, program)) {
    //         printf("✅ Complete ownership analysis with escape analysis: success\n");
    //     } else {
    //         printf("⚠️  Ownership analysis completed with warnings\n");
    //     }
    // }

    return checker->error_count == 0;
}

int type_check_declaration(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl) return 0;
    
    switch (decl->type) {
        case AST_FUNC_DECL:
            return type_check_function_decl(checker, decl);
        case AST_VAR_DECL:
            return type_check_var_decl(checker, decl);
        case AST_CONST_DECL:
            return type_check_const_decl(checker, decl);
        case AST_TYPE_DECL:
            return type_check_type_decl(checker, decl);
        case AST_CONCEPT_DECL:
            return type_check_concept_decl(checker, decl);
        default:
            type_error(checker, decl->pos, "Unknown declaration type");
            return 0;
    }
}

int type_check_function_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_FUNC_DECL) return 0;

    FuncDeclNode* func = (FuncDeclNode*)decl;

    // First, collect parameter types and return type
    Type** param_types = NULL;
    size_t param_count = 0;

    // Count parameters first
    if (func->params) {
        ASTNode* param = func->params;
        while (param) {
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* param_decl = (VarDeclNode*)param;
                param_count += param_decl->name_count;
            }
            param = param->next;
        }
    }

    // Allocate param types array
    if (param_count > 0) {
        param_types = malloc(sizeof(Type*) * param_count);
        size_t idx = 0;

        ASTNode* param = func->params;
        while (param) {
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* param_decl = (VarDeclNode*)param;

                // Get parameter type
                Type* param_type = NULL;
                if (param_decl->type) {
                    param_type = type_from_ast(checker, param_decl->type);
                } else {
                    param_type = type_checker_get_builtin(checker, TYPE_INT32); // Default type
                }

                // Add to param_types array
                for (size_t i = 0; i < param_decl->name_count; i++) {
                    param_types[idx++] = param_type;
                }
            }
            param = param->next;
        }
    }

    // Get function return type
    Type* return_type = NULL;
    if (func->return_type) {
        return_type = type_from_ast(checker, func->return_type);
        // If type_from_ast returns NULL, default to void
        if (!return_type) {
            return_type = type_checker_get_builtin(checker, TYPE_VOID);
        }
    } else {
        return_type = type_checker_get_builtin(checker, TYPE_VOID);
    }

    // Create function name (mangled for methods)
    char* func_name = func->name;
    char* mangled_name = NULL;
    if (func->receiver_type) {
        // Get the receiver type for mangling
        Type* receiver_type = type_from_ast(checker, func->receiver_type);
        const char* type_name = NULL;

        if (receiver_type && receiver_type->kind == TYPE_STRUCT) {
            type_name = receiver_type->data.struct_type.name;
        } else if (receiver_type && receiver_type->kind == TYPE_POINTER &&
                   receiver_type->data.pointer.pointee_type &&
                   receiver_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
            type_name = receiver_type->data.pointer.pointee_type->data.struct_type.name;
        }

        if (type_name) {
            // Mangle as: TypeName_methodName
            size_t len = strlen(type_name) + strlen(func->name) + 2;
            mangled_name = malloc(len);
            snprintf(mangled_name, len, "%s_%s", type_name, func->name);
            func_name = mangled_name;
        }
    }

    // Create proper function type with actual signature
    Type* func_type = type_function(param_types, param_count, return_type);
    Variable* func_var = variable_new(func_name, func_type, func->base.pos);
    if (func_var) {
        func_var->is_initialized = 1;
        if (!scope_add_variable(checker->current_scope, func_var)) {
            type_error(checker, func->base.pos, "Function '%s' already declared", func_name);
            variable_free(func_var);
            if (mangled_name) free(mangled_name);
            free(param_types);
            return 0;
        }
    }
    if (mangled_name) free(mangled_name);

    // Create new scope for function
    scope_push(checker);

    // Add receiver to scope if this is a method
    if (func->receiver_name && func->receiver_type) {
        Type* receiver_type = type_from_ast(checker, func->receiver_type);
        if (receiver_type) {
            Variable* receiver_var = variable_new(func->receiver_name, receiver_type, func->base.pos);
            if (receiver_var) {
                receiver_var->is_initialized = 1; // Receiver is always initialized
                scope_add_variable(checker->current_scope, receiver_var);
            }
        }
    }

    // Add function parameters to the function scope
    if (func->params) {
        ASTNode* param = func->params;
        while (param) {
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* param_decl = (VarDeclNode*)param;

                // Get parameter type
                Type* param_type = NULL;
                if (param_decl->type) {
                    param_type = type_from_ast(checker, param_decl->type);
                } else {
                    param_type = type_checker_get_builtin(checker, TYPE_INT32); // Default type
                }

                if (param_type) {
                    for (size_t i = 0; i < param_decl->name_count; i++) {
                        Variable* param_var = variable_new(param_decl->names[i], param_type, param_decl->base.pos);
                        if (param_var) {
                            param_var->is_initialized = 1; // Parameters are always initialized
                            scope_add_variable(checker->current_scope, param_var);
                        }
                    }
                }
            }
            param = param->next;
        }
    }

    // Add named return parameters to the function scope (Go semantics: zero-initialized)
    if (func->named_returns) {
        ASTNode* named_return = func->named_returns;
        while (named_return) {
            if (named_return->type == AST_VAR_DECL) {
                VarDeclNode* return_decl = (VarDeclNode*)named_return;

                // Get return parameter type
                Type* return_type_param = NULL;
                if (return_decl->type) {
                    return_type_param = type_from_ast(checker, return_decl->type);
                } else {
                    return_type_param = type_checker_get_builtin(checker, TYPE_INT32); // Default type
                }

                if (return_type_param) {
                    for (size_t i = 0; i < return_decl->name_count; i++) {
                        Variable* return_var = variable_new(return_decl->names[i], return_type_param, return_decl->base.pos);
                        if (return_var) {
                            return_var->is_initialized = 1; // Named returns are zero-initialized
                            scope_add_variable(checker->current_scope, return_var);
                        }
                    }
                }
            }
            named_return = named_return->next;
        }
    }

    // Set current function return type for return statement validation
    Type* saved_return_type = checker->current_function_return_type;
    checker->current_function_return_type = return_type;

    // Type check function body
    int result = 1;
    if (func->body) {
        result = type_check_statement(checker, func->body);
    }

    // Restore previous function return type
    checker->current_function_return_type = saved_return_type;

    scope_pop(checker);
    return result;
}

int type_check_var_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_VAR_DECL) return 0;
    
    VarDeclNode* var_decl = (VarDeclNode*)decl;
    
    // Get declared type
    Type* declared_type = NULL;
    if (var_decl->type) {
        declared_type = type_from_ast(checker, var_decl->type);
        if (!declared_type) {
            type_error(checker, var_decl->base.pos, "Invalid type in variable declaration");
            return 0;
        }
    }
    
    // Check initial values
    Type* inferred_type = NULL;
    if (var_decl->values) {
        inferred_type = type_check_expression(checker, var_decl->values);
        if (!inferred_type) {
            type_error(checker, var_decl->base.pos, "Invalid initializer expression");
            return 0;
        }
    }
    
    // Determine final type
    Type* final_type = declared_type;
    if (!final_type) {
        final_type = inferred_type;
    } else if (inferred_type) {
        // Check compatibility
        if (!type_compatible(inferred_type, declared_type)) {
            type_error(checker, var_decl->base.pos, 
                      "Cannot assign %s to %s", 
                      type_to_string(inferred_type), 
                      type_to_string(declared_type));
            return 0;
        }
    }
    
    if (!final_type) {
        type_error(checker, var_decl->base.pos, 
                  "Variable declaration must have either type or initializer");
        return 0;
    }
    
    // Store the type on the AST node for code generation
    var_decl->base.node_type = final_type;

    // Handle multiple assignment from tuple
    if (var_decl->name_count > 1 && final_type && final_type->kind == TYPE_TUPLE) {
        // Check that the number of names matches the number of tuple elements
        if (var_decl->name_count != final_type->data.tuple.element_count) {
            type_error(checker, var_decl->base.pos,
                      "Assignment mismatch: %zu variables but %zu values",
                      var_decl->name_count, final_type->data.tuple.element_count);
            return 0;
        }

        // Add each variable with its corresponding tuple element type
        for (size_t i = 0; i < var_decl->name_count; i++) {
            // Skip underscore (ignored values)
            if (strcmp(var_decl->names[i], "_") == 0) {
                continue;
            }

            Type* elem_type = final_type->data.tuple.element_types[i];
            Variable* var = variable_new(var_decl->names[i], elem_type, var_decl->base.pos);
            if (!var) {
                type_error(checker, var_decl->base.pos, "Memory allocation failed");
                return 0;
            }

            var->ownership = var_decl->ownership;
            var->is_initialized = 1;

            if (!scope_add_variable(checker->current_scope, var)) {
                type_error(checker, var_decl->base.pos,
                          "Variable '%s' already declared in this scope", var_decl->names[i]);
                variable_free(var);
                return 0;
            }
        }
    } else {
        // Single variable or non-tuple type
        // Add variables to scope
        for (size_t i = 0; i < var_decl->name_count; i++) {
            Variable* var = variable_new(var_decl->names[i], final_type, var_decl->base.pos);
            if (!var) {
                type_error(checker, var_decl->base.pos, "Memory allocation failed");
                return 0;
            }

            var->ownership = var_decl->ownership;
            // All declared variables in Go/Goo are zero-initialized
            // (numbers→0, arrays→all zeros, pointers→nil, etc.)
            var->is_initialized = 1;

            if (!scope_add_variable(checker->current_scope, var)) {
                type_error(checker, var_decl->base.pos,
                          "Variable '%s' already declared in this scope", var_decl->names[i]);
                variable_free(var);
                return 0;
            }
        }
    }
    
    return 1;
}

int type_check_const_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_CONST_DECL) return 0;

    ConstDeclNode* const_decl = (ConstDeclNode*)decl;
    
    // Constants must have values
    if (!const_decl->values) {
        type_error(checker, const_decl->base.pos, "Constant declaration must have initializer");
        return 0;
    }
    
    // Type check the value
    Type* value_type = type_check_expression(checker, const_decl->values);
    if (!value_type) {
        return 0;
    }
    
    // Check declared type if present
    Type* declared_type = NULL;
    if (const_decl->type) {
        declared_type = type_from_ast(checker, const_decl->type);
        if (!declared_type) {
            return 0;
        }

        if (!type_compatible(value_type, declared_type)) {
            type_error(checker, const_decl->base.pos,
                      "Cannot assign %s to %s",
                      type_to_string(value_type),
                      type_to_string(declared_type));
            return 0;
        }
        value_type = declared_type;
    }

    // Store the type on the AST node for code generation
    const_decl->base.node_type = value_type;

    // Add constants to scope (treated as immutable variables)
    for (size_t i = 0; i < const_decl->name_count; i++) {
        Variable* var = variable_new(const_decl->names[i], value_type, const_decl->base.pos);
        if (!var) return 0;
        
        var->mutability = MUTABILITY_IMMUTABLE;
        var->is_initialized = 1;
        
        if (!scope_add_variable(checker->current_scope, var)) {
            type_error(checker, const_decl->base.pos,
                      "Constant '%s' already declared in this scope", const_decl->names[i]);
            variable_free(var);
            return 0;
        }
    }

    return 1;
}

int type_check_type_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_TYPE_DECL) return 0;

    TypeDeclNode* type_decl = (TypeDeclNode*)decl;

    // Check if this is a struct type declaration
    if (type_decl->type && type_decl->type->type == AST_STRUCT_TYPE) {
        StructTypeNode* struct_node = (StructTypeNode*)type_decl->type;

        // Create struct fields
        StructField* fields = NULL;
        if (struct_node->field_count > 0) {
            fields = malloc(sizeof(StructField) * struct_node->field_count);

            for (size_t i = 0; i < struct_node->field_count; i++) {
                fields[i].name = strdup(struct_node->field_names[i]);
                fields[i].type = type_from_ast(checker, struct_node->field_types[i]);
                fields[i].offset = i;  // Will be computed properly in codegen
                fields[i].ownership = OWNERSHIP_OWNED;
                fields[i].mutability = MUTABILITY_MUTABLE;

                if (!fields[i].type) {
                    // Cleanup on error
                    for (size_t j = 0; j < i; j++) {
                        free(fields[j].name);
                    }
                    free(fields);
                    return 0;
                }
            }
        }

        // Create the struct type
        Type* struct_type = type_new(TYPE_STRUCT);
        struct_type->data.struct_type.fields = fields;
        struct_type->data.struct_type.field_count = struct_node->field_count;
        struct_type->data.struct_type.name = strdup(type_decl->name);

        // Register the type as a variable so it can be looked up
        Variable* type_var = variable_new(type_decl->name, struct_type, type_decl->base.pos);
        if (type_var) {
            type_var->is_initialized = 1;
            if (!scope_add_variable(checker->current_scope, type_var)) {
                type_error(checker, type_decl->base.pos,
                          "Type '%s' already declared in this scope", type_decl->name);
                variable_free(type_var);
                return 0;
            }
        }

        return 1;
    }

    // TODO: Handle other type declarations (aliases, etc.)
    return 1;
}

int type_check_concept_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_CONCEPT_DECL) return 0;
    
    ConceptDeclNode* concept = (ConceptDeclNode*)decl;
    
    // For now, just register the concept in the global scope
    // TODO: Implement full concept validation and constraint checking
    
    // Create a concept type to represent this concept definition
    Type* concept_type = type_concept(concept->name);
    Variable* concept_var = variable_new(concept->name, concept_type, concept->base.pos);
    if (concept_var) {
        concept_var->is_initialized = 1;
        if (!scope_add_variable(checker->current_scope, concept_var)) {
            type_error(checker, concept->base.pos, "Concept '%s' already declared", concept->name);
            variable_free(concept_var);
            return 0;
        }
    }
    
    return 1;
}

int type_check_statement(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt) return 0;

    switch (stmt->type) {
        case AST_BLOCK_STMT:
            return type_check_block_stmt(checker, stmt);
        case AST_EXPR_STMT:
            return type_check_expr_stmt(checker, stmt);
        case AST_VAR_DECL:
            return type_check_var_decl(checker, stmt);
        case AST_CONST_DECL:
            return type_check_const_decl(checker, stmt);
        case AST_TYPE_DECL:
            return 1;  // Type decls handled elsewhere, accept in statement context
        case AST_IF_STMT:
            return type_check_if_stmt(checker, stmt);
        case AST_IF_LET_STMT:
        case AST_DEFER_STMT:
        case AST_UNSAFE_STMT:
        case AST_ASM_STMT:
        case AST_PARALLEL_FOR:
        case AST_COMPTIME_BLOCK:
            return 1;  // TODO: Implement full type checking for these
        case AST_FOR_STMT:
            return type_check_for_stmt(checker, stmt);
        case AST_RANGE_STMT:
            return type_check_range_stmt(checker, stmt);
        case AST_SWITCH_STMT:
            return type_check_switch_stmt(checker, stmt);
        case AST_RETURN_STMT:
            return type_check_return_stmt(checker, stmt);
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            return 1;  // Always valid
        case AST_GO_STMT:
            return type_check_go_stmt(checker, stmt);
        case AST_SELECT_STMT:
            return type_check_select_stmt(checker, stmt);
        default:
            type_error(checker, stmt->pos, "Unknown statement type: %d", stmt->type);
            return 0;
    }
}

int type_check_block_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_BLOCK_STMT) return 0;
    
    BlockStmtNode* block = (BlockStmtNode*)stmt;
    
    scope_push(checker);
    
    int result = 1;
    ASTNode* current = block->statements;
    while (current) {
        if (!type_check_statement(checker, current)) {
            result = 0;
        }
        current = current->next;
    }
    
    scope_pop(checker);
    return result;
}

int type_check_expr_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_EXPR_STMT) return 0;
    
    ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
    Type* type = type_check_expression(checker, expr_stmt->expr);
    return type != NULL;
}

int type_check_if_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_IF_STMT) return 0;
    
    IfStmtNode* if_stmt = (IfStmtNode*)stmt;
    
    // Check condition
    Type* cond_type = type_check_expression(checker, if_stmt->condition);
    if (!cond_type) return 0;
    
    // Condition must be boolean
    if (cond_type->kind != TYPE_BOOL) {
        type_error(checker, if_stmt->condition->pos, 
                  "If condition must be boolean, got %s", type_to_string(cond_type));
        return 0;
    }
    
    // Check then branch
    if (!type_check_statement(checker, if_stmt->then_stmt)) {
        return 0;
    }
    
    // Check else branch if present
    if (if_stmt->else_stmt) {
        if (!type_check_statement(checker, if_stmt->else_stmt)) {
            return 0;
        }
    }
    
    return 1;
}

int type_check_for_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_FOR_STMT) return 0;
    
    ForStmtNode* for_stmt = (ForStmtNode*)stmt;
    
    scope_push(checker);
    
    int result = 1;
    
    // Check initialization
    if (for_stmt->init) {
        if (!type_check_statement(checker, for_stmt->init)) {
            result = 0;
        }
    }
    
    // Check condition
    if (for_stmt->condition) {
        Type* cond_type = type_check_expression(checker, for_stmt->condition);
        if (!cond_type || cond_type->kind != TYPE_BOOL) {
            type_error(checker, for_stmt->condition->pos,
                      "For condition must be boolean");
            result = 0;
        }
    }
    
    // Check post statement
    if (for_stmt->post) {
        if (!type_check_statement(checker, for_stmt->post)) {
            result = 0;
        }
    }
    
    // Check body
    if (for_stmt->body) {
        if (!type_check_statement(checker, for_stmt->body)) {
            result = 0;
        }
    }
    
    scope_pop(checker);
    return result;
}

int type_check_range_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_RANGE_STMT) return 0;

    RangeStmtNode* range_stmt = (RangeStmtNode*)stmt;

    // Type check the range expression
    Type* range_type = type_check_expression(checker, range_stmt->range_expr);
    if (!range_type) {
        type_error(checker, range_stmt->range_expr->pos, "Invalid range expression");
        return 0;
    }

    // Determine element type based on what we're ranging over
    Type* element_type = NULL;
    Type* index_type = type_checker_get_builtin(checker, TYPE_INT32);

    if (range_type->kind == TYPE_ARRAY) {
        element_type = range_type->data.array.element_type;
    } else if (range_type->kind == TYPE_SLICE) {
        element_type = range_type->data.slice.element_type;
    } else {
        type_error(checker, range_stmt->range_expr->pos,
                  "Can only range over arrays and slices, got %s",
                  type_to_string(range_type));
        return 0;
    }

    // Create new scope for loop variables
    scope_push(checker);

    int result = 1;

    // Add index variable to scope (unless it's "_")
    if (range_stmt->index_var && strcmp(range_stmt->index_var, "_") != 0) {
        Variable* index_var = variable_new(range_stmt->index_var, index_type, range_stmt->base.pos);
        if (index_var) {
            index_var->is_initialized = 1;  // Loop variables are initialized
            if (!scope_add_variable(checker->current_scope, index_var)) {
                type_error(checker, range_stmt->base.pos,
                          "Variable '%s' already declared", range_stmt->index_var);
                variable_free(index_var);
                result = 0;
            }
        }
    }

    // Add value variable to scope (unless it's "_" or NULL)
    if (range_stmt->value_var && strcmp(range_stmt->value_var, "_") != 0) {
        Variable* value_var = variable_new(range_stmt->value_var, element_type, range_stmt->base.pos);
        if (value_var) {
            value_var->is_initialized = 1;  // Loop variables are initialized
            if (!scope_add_variable(checker->current_scope, value_var)) {
                type_error(checker, range_stmt->base.pos,
                          "Variable '%s' already declared", range_stmt->value_var);
                variable_free(value_var);
                result = 0;
            }
        }
    }

    // Type check the body
    if (range_stmt->body) {
        if (!type_check_statement(checker, range_stmt->body)) {
            result = 0;
        }
    }

    scope_pop(checker);
    return result;
}

int type_check_switch_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_SWITCH_STMT) return 0;

    SwitchStmtNode* switch_stmt = (SwitchStmtNode*)stmt;
    Type* tag_type = NULL;

    // If there's a tag expression, type check it
    if (switch_stmt->tag) {
        tag_type = type_check_expression(checker, switch_stmt->tag);
        if (!tag_type) return 0;
    }

    int result = 1;

    // Type check each case clause
    ASTNode* case_node = switch_stmt->cases;
    while (case_node) {
        if (case_node->type != AST_CASE_CLAUSE) {
            type_error(checker, case_node->pos, "Expected case clause in switch");
            result = 0;
            case_node = case_node->next;
            continue;
        }

        CaseClauseNode* case_clause = (CaseClauseNode*)case_node;

        // For non-default cases, check case values
        if (!case_clause->is_default && case_clause->values) {
            ASTNode* value_node = case_clause->values;
            while (value_node) {
                Type* value_type = type_check_expression(checker, value_node);
                if (!value_type) {
                    result = 0;
                } else if (tag_type) {
                    // With tag: case values must match tag type
                    if (!type_equals(value_type, tag_type)) {
                        type_error(checker, value_node->pos,
                                  "Case value type %s does not match switch tag type %s",
                                  type_to_string(value_type), type_to_string(tag_type));
                        result = 0;
                    }
                } else {
                    // Without tag: case values must be boolean
                    if (value_type->kind != TYPE_BOOL) {
                        type_error(checker, value_node->pos,
                                  "Case value in tagless switch must be boolean, got %s",
                                  type_to_string(value_type));
                        result = 0;
                    }
                }
                value_node = value_node->next;
            }
        }

        // Type check case body
        if (case_clause->body) {
            ASTNode* body_stmt = case_clause->body;
            while (body_stmt) {
                if (!type_check_statement(checker, body_stmt)) {
                    result = 0;
                }
                body_stmt = body_stmt->next;
            }
        }

        case_node = case_node->next;
    }

    return result;
}

int type_check_return_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_RETURN_STMT) return 0;

    ReturnStmtNode* ret_stmt = (ReturnStmtNode*)stmt;

    // Check return value if present
    if (ret_stmt->values) {
        // Count return values
        ASTNode* current = ret_stmt->values;
        size_t value_count = 0;
        while (current) {
            value_count++;
            current = current->next;
        }

        Type* actual_return_type = NULL;
        if (value_count > 1) {
            // Multiple return values - build tuple type
            Type** elem_types = malloc(sizeof(Type*) * value_count);
            if (!elem_types) return 0;

            current = ret_stmt->values;
            for (size_t i = 0; i < value_count; i++) {
                elem_types[i] = type_check_expression(checker, current);
                if (!elem_types[i]) {
                    free(elem_types);
                    return 0;
                }
                current = current->next;
            }

            actual_return_type = type_tuple(elem_types, value_count);
        } else {
            // Single return value
            actual_return_type = type_check_expression(checker, ret_stmt->values);
            if (!actual_return_type) return 0;
        }

        // Validate against function's declared return type
        if (checker->current_function_return_type) {
            if (!type_compatible(actual_return_type, checker->current_function_return_type)) {
                type_error(checker, stmt->pos,
                    "Return type mismatch: expected %s, got %s",
                    type_to_string(checker->current_function_return_type),
                    type_to_string(actual_return_type));
                return 0;
            }
        }
    } else {
        // No return value - check if function expects void
        if (checker->current_function_return_type &&
            checker->current_function_return_type->kind != TYPE_VOID) {
            type_error(checker, stmt->pos,
                "Function must return a value of type %s",
                type_to_string(checker->current_function_return_type));
            return 0;
        }
    }

    return 1;
}

int type_check_go_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_GO_STMT) return 0;
    
    GoStmtNode* go_stmt = (GoStmtNode*)stmt;
    
    // Type check the function call expression
    if (go_stmt->call) {
        Type* call_type = type_check_expression(checker, go_stmt->call);
        if (!call_type) return 0;
        
        // TODO: Validate that the expression is a function call
    }
    
    return 1;
}

int type_check_select_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_SELECT_STMT) return 0;
    
    SelectStmtNode* select_stmt = (SelectStmtNode*)stmt;
    
    // Type check each case
    ASTNode* case_node = select_stmt->cases;
    while (case_node) {
        // TODO: Implement proper select case type checking
        // For now, just accept it as valid
        case_node = case_node->next;
    }
    
    return 1;
}

// Forward declarations for helper functions
Type* type_from_ast(TypeChecker* checker, ASTNode* type_node);
Type* type_check_expression(TypeChecker* checker, ASTNode* expr);

// Helper function to convert AST type nodes to Type structures
Type* type_from_ast(TypeChecker* checker, ASTNode* type_node) {
    if (!checker || !type_node) return NULL;

    switch (type_node->type) {
        case AST_IDENTIFIER: {
            // Handle type identifiers (for make_chan, etc.)
            IdentifierNode* ident = (IdentifierNode*)type_node;
            
            // Map basic type names to TypeKind
            if (strcmp(ident->name, "void") == 0) return type_checker_get_builtin(checker, TYPE_VOID);
            if (strcmp(ident->name, "bool") == 0) return type_checker_get_builtin(checker, TYPE_BOOL);
            if (strcmp(ident->name, "int8") == 0) return type_checker_get_builtin(checker, TYPE_INT8);
            if (strcmp(ident->name, "int16") == 0) return type_checker_get_builtin(checker, TYPE_INT16);
            if (strcmp(ident->name, "int32") == 0) return type_checker_get_builtin(checker, TYPE_INT32);
            if (strcmp(ident->name, "int64") == 0) return type_checker_get_builtin(checker, TYPE_INT64);
            if (strcmp(ident->name, "int") == 0) return type_checker_get_builtin(checker, TYPE_INT32);  // Default int
            if (strcmp(ident->name, "uint8") == 0) return type_checker_get_builtin(checker, TYPE_UINT8);
            if (strcmp(ident->name, "uint16") == 0) return type_checker_get_builtin(checker, TYPE_UINT16);
            if (strcmp(ident->name, "uint32") == 0) return type_checker_get_builtin(checker, TYPE_UINT32);
            if (strcmp(ident->name, "uint64") == 0) return type_checker_get_builtin(checker, TYPE_UINT64);
            if (strcmp(ident->name, "uint") == 0) return type_checker_get_builtin(checker, TYPE_UINT32);  // Default uint
            if (strcmp(ident->name, "float32") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT32);
            if (strcmp(ident->name, "float64") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);
            if (strcmp(ident->name, "float") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);  // Default float
            if (strcmp(ident->name, "string") == 0) return type_checker_get_builtin(checker, TYPE_STRING);
            if (strcmp(ident->name, "char") == 0) return type_checker_get_builtin(checker, TYPE_CHAR);
            if (strcmp(ident->name, "byte") == 0) return type_checker_get_builtin(checker, TYPE_UINT8);

            // Look up user-defined types (structs, interfaces, etc.)
            Variable* type_var = scope_lookup_variable(checker->current_scope, ident->name);
            if (type_var && type_var->type) {
                return type_var->type;
            }

            type_error(checker, type_node->pos, "Unknown type '%s'", ident->name);
            return NULL;
        }
        case AST_BASIC_TYPE: {
            BasicTypeNode* basic = (BasicTypeNode*)type_node;
            
            // Map basic type names to TypeKind
            if (strcmp(basic->name, "void") == 0) return type_checker_get_builtin(checker, TYPE_VOID);
            if (strcmp(basic->name, "bool") == 0) return type_checker_get_builtin(checker, TYPE_BOOL);
            if (strcmp(basic->name, "int8") == 0) return type_checker_get_builtin(checker, TYPE_INT8);
            if (strcmp(basic->name, "int16") == 0) return type_checker_get_builtin(checker, TYPE_INT16);
            if (strcmp(basic->name, "int32") == 0) return type_checker_get_builtin(checker, TYPE_INT32);
            if (strcmp(basic->name, "int64") == 0) return type_checker_get_builtin(checker, TYPE_INT64);
            if (strcmp(basic->name, "int") == 0) return type_checker_get_builtin(checker, TYPE_INT32);  // Default int
            if (strcmp(basic->name, "uint8") == 0) return type_checker_get_builtin(checker, TYPE_UINT8);
            if (strcmp(basic->name, "uint16") == 0) return type_checker_get_builtin(checker, TYPE_UINT16);
            if (strcmp(basic->name, "uint32") == 0) return type_checker_get_builtin(checker, TYPE_UINT32);
            if (strcmp(basic->name, "uint64") == 0) return type_checker_get_builtin(checker, TYPE_UINT64);
            if (strcmp(basic->name, "uint") == 0) return type_checker_get_builtin(checker, TYPE_UINT32);  // Default uint
            if (strcmp(basic->name, "float32") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT32);
            if (strcmp(basic->name, "float64") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);
            if (strcmp(basic->name, "float") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);  // Default float
            if (strcmp(basic->name, "string") == 0) return type_checker_get_builtin(checker, TYPE_STRING);
            if (strcmp(basic->name, "char") == 0) return type_checker_get_builtin(checker, TYPE_CHAR);
            if (strcmp(basic->name, "byte") == 0) return type_checker_get_builtin(checker, TYPE_UINT8);

            // Look up user-defined types (structs, interfaces, etc.)
            Variable* type_var = scope_lookup_variable(checker->current_scope, basic->name);
            if (type_var && type_var->type) {
                return type_var->type;
            }

            type_error(checker, type_node->pos, "Unknown type '%s'", basic->name);
            return NULL;
        }
        
        case AST_ARRAY_TYPE: {
            ArrayTypeNode* array = (ArrayTypeNode*)type_node;
            Type* element_type = type_from_ast(checker, array->element_type);
            if (!element_type) return NULL;
            
            // TODO: Evaluate length expression
            size_t length = 10;  // Placeholder
            return type_array(element_type, length);
        }
        
        case AST_SLICE_TYPE: {
            SliceTypeNode* slice = (SliceTypeNode*)type_node;
            Type* element_type = type_from_ast(checker, slice->element_type);
            if (!element_type) return NULL;
            return type_slice(element_type);
        }
        
        case AST_MAP_TYPE: {
            MapTypeNode* map = (MapTypeNode*)type_node;
            Type* key_type = type_from_ast(checker, map->key_type);
            Type* value_type = type_from_ast(checker, map->value_type);
            if (!key_type || !value_type) return NULL;
            return type_map(key_type, value_type);
        }
        
        case AST_CHAN_TYPE: {
            ChanTypeNode* chan = (ChanTypeNode*)type_node;
            Type* element_type = type_from_ast(checker, chan->element_type);
            if (!element_type) return NULL;
            return type_channel(element_type, chan->pattern);
        }
        
        case AST_POINTER_TYPE: {
            PointerTypeNode* ptr = (PointerTypeNode*)type_node;
            Type* pointee_type = type_from_ast(checker, ptr->element_type);
            if (!pointee_type) return NULL;
            return type_pointer(pointee_type);
        }
        
        case AST_REFERENCE_TYPE: {
            ReferenceTypeNode* ref = (ReferenceTypeNode*)type_node;
            Type* referenced_type = type_from_ast(checker, ref->element_type);
            if (!referenced_type) return NULL;
            return type_reference(referenced_type, ref->is_mutable);
        }
        
        case AST_ERROR_UNION_TYPE: {
            ErrorUnionTypeNode* error_union = (ErrorUnionTypeNode*)type_node;
            Type* value_type = type_from_ast(checker, error_union->value_type);
            if (!value_type) return NULL;

            Type* error_type = NULL;
            if (error_union->error_type) {
                error_type = type_from_ast(checker, error_union->error_type);
                if (!error_type) return NULL;
            }

            return type_error_union(value_type, error_type);
        }
        
        case AST_NULLABLE_TYPE: {
            NullableTypeNode* nullable = (NullableTypeNode*)type_node;
            Type* base_type = type_from_ast(checker, nullable->base_type);
            if (!base_type) return NULL;
            return type_nullable(base_type);
        }

        case AST_TUPLE_TYPE: {
            TupleTypeNode* tuple_ast = (TupleTypeNode*)type_node;

            // Convert each element type
            Type** element_types = malloc(sizeof(Type*) * tuple_ast->element_count);
            if (!element_types) return NULL;

            for (size_t i = 0; i < tuple_ast->element_count; i++) {
                element_types[i] = type_from_ast(checker, tuple_ast->element_types[i]);
                if (!element_types[i]) {
                    // Clean up on error
                    free(element_types);
                    return NULL;
                }
            }

            return type_tuple(element_types, tuple_ast->element_count);
        }

        default:
            type_error(checker, type_node->pos, "Invalid type node");
            return NULL;
    }
}