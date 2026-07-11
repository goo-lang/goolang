#include "dependent_types.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Utility for strdup if not available
static char* str_dup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

// Use our portable version
#define strdup str_dup

// =============================================================================
// Context Management
// =============================================================================

DependentTypeContext* dependent_type_context_new(TypeChecker* type_checker) {
    DependentTypeContext* context = xmalloc(sizeof(DependentTypeContext));
    if (!context) return NULL;
    
    memset(context, 0, sizeof(DependentTypeContext));
    
    context->type_checker = type_checker;
    context->solver = constraint_solver_new(context);
    
    // Initialize type storage
    context->dependent_type_capacity = 64;
    context->dependent_types = malloc(sizeof(DependentType*) * context->dependent_type_capacity);
    if (context->dependent_types) {
        memset(context->dependent_types, 0, sizeof(DependentType*) * context->dependent_type_capacity);
    }
    
    // Initialize constraint storage
    context->constraint_capacity = 128;
    context->active_constraints = malloc(sizeof(TypeConstraint*) * context->constraint_capacity);
    if (context->active_constraints) {
        memset(context->active_constraints, 0, sizeof(TypeConstraint*) * context->constraint_capacity);
    }
    
    // Set default configuration
    context->enable_dependent_types = 1;
    context->enable_refinement_types = 1;
    context->enable_constraint_inference = 1;
    context->strict_constraint_checking = 1;
    
    // Register built-in types
    register_builtin_dependent_types(context);
    register_builtin_refinement_types(context);
    
    return context;
}

void dependent_type_context_free(DependentTypeContext* context) {
    if (!context) return;
    
    // Free dependent types
    if (context->dependent_types) {
        for (size_t i = 0; i < context->dependent_type_count; i++) {
            if (context->dependent_types[i]) {
                dependent_type_free(context->dependent_types[i]);
            }
        }
        free(context->dependent_types);
    }
    
    // Free refinement types
    RefinementType* refinement = context->refinement_types;
    while (refinement) {
        RefinementType* next = refinement->next;
        refinement_type_free(refinement);
        refinement = next;
    }
    
    // Free active constraints
    if (context->active_constraints) {
        for (size_t i = 0; i < context->constraint_count; i++) {
            if (context->active_constraints[i]) {
                type_constraint_free(context->active_constraints[i]);
            }
        }
        free(context->active_constraints);
    }
    
    // Free type parameters
    TypeParameter* param = context->type_env;
    while (param) {
        TypeParameter* next = param->next;
        type_parameter_free(param);
        param = next;
    }
    
    // Free constraint solver
    if (context->solver) {
        constraint_solver_free(context->solver);
    }
    
    free(context);
}

// =============================================================================
// Dependent Type Management
// =============================================================================

DependentType* dependent_type_new(DependentTypeKind kind, const char* name) {
    DependentType* type = xmalloc(sizeof(DependentType));
    if (!type) return NULL;
    
    memset(type, 0, sizeof(DependentType));
    type->kind = kind;
    type->name = name ? strdup(name) : NULL;
    
    return type;
}

void dependent_type_free(DependentType* type) {
    if (!type) return;
    
    free(type->name);
    
    // Free type parameters
    TypeParameter* param = type->parameters;
    while (param) {
        TypeParameter* next = param->next;
        type_parameter_free(param);
        param = next;
    }
    
    // Free constraints
    TypeConstraint* constraint = type->constraints;
    while (constraint) {
        TypeConstraint* next = constraint->next;
        type_constraint_free(constraint);
        constraint = next;
    }
    
    // Free kind-specific data
    switch (type->kind) {
        case DEPENDENT_BOUNDED_VEC:
            free(type->data.bounded_vec.capacity_param);
            break;
        case DEPENDENT_BOUNDED_INT:
            free(type->data.bounded_int.min_param);
            free(type->data.bounded_int.max_param);
            break;
        case DEPENDENT_SIZED_ARRAY:
            free(type->data.sized_array.size_param);
            break;
        case DEPENDENT_REFINED_TYPE:
            // refinements are freed above in constraints loop
            break;
        case DEPENDENT_INDEXED_TYPE:
        case DEPENDENT_FUNCTION_TYPE:
            // Additional cleanup if needed
            break;
    }
    
    free(type);
}

// =============================================================================
// Bounded Vector Type: BoundedVec<T, N>
// =============================================================================

DependentType* create_bounded_vec_type(Type* element_type, int64_t capacity) {
    DependentType* type = dependent_type_new(DEPENDENT_BOUNDED_VEC, "BoundedVec");
    if (!type) return NULL;
    
    type->base_type = element_type;
    type->data.bounded_vec.element_type = element_type;
    type->data.bounded_vec.capacity = capacity;
    type->data.bounded_vec.is_capacity_dynamic = 0;
    
    // Add capacity constraint
    TypeConstraint* capacity_constraint = create_size_constraint(DEP_CONSTRAINT_SIZE_LE, capacity);
    capacity_constraint->next = type->constraints;
    type->constraints = capacity_constraint;
    
    return type;
}

DependentType* create_dynamic_bounded_vec_type(Type* element_type, const char* capacity_param) {
    DependentType* type = dependent_type_new(DEPENDENT_BOUNDED_VEC, "BoundedVec");
    if (!type) return NULL;
    
    type->base_type = element_type;
    type->data.bounded_vec.element_type = element_type;
    type->data.bounded_vec.is_capacity_dynamic = 1;
    type->data.bounded_vec.capacity_param = strdup(capacity_param);
    
    // Create type parameter for capacity
    TypeParameter* param = type_parameter_new(TYPE_PARAM_VALUE, capacity_param);
    if (param) {
        param->next = type->parameters;
        type->parameters = param;
    }
    
    return type;
}

// =============================================================================
// Bounded Integer Type: BoundedInt<Min, Max>
// =============================================================================

DependentType* create_bounded_int_type(int64_t min_value, int64_t max_value) {
    DependentType* type = dependent_type_new(DEPENDENT_BOUNDED_INT, "BoundedInt");
    if (!type) return NULL;
    
    // Use basic integer type as base
    type->base_type = NULL; // Would set to int type
    type->data.bounded_int.min_value = min_value;
    type->data.bounded_int.max_value = max_value;
    type->data.bounded_int.is_min_dynamic = 0;
    type->data.bounded_int.is_max_dynamic = 0;
    
    // Add range constraint
    TypeConstraint* range_constraint = create_range_constraint(min_value, max_value);
    range_constraint->next = type->constraints;
    type->constraints = range_constraint;
    
    return type;
}

DependentType* create_dynamic_bounded_int_type(const char* min_param, const char* max_param) {
    DependentType* type = dependent_type_new(DEPENDENT_BOUNDED_INT, "BoundedInt");
    if (!type) return NULL;
    
    type->base_type = NULL; // Would set to int type
    type->data.bounded_int.is_min_dynamic = 1;
    type->data.bounded_int.is_max_dynamic = 1;
    type->data.bounded_int.min_param = strdup(min_param);
    type->data.bounded_int.max_param = strdup(max_param);
    
    // Create type parameters
    TypeParameter* min_param_obj = type_parameter_new(TYPE_PARAM_VALUE, min_param);
    TypeParameter* max_param_obj = type_parameter_new(TYPE_PARAM_VALUE, max_param);
    
    if (min_param_obj && max_param_obj) {
        max_param_obj->next = type->parameters;
        min_param_obj->next = max_param_obj;
        type->parameters = min_param_obj;
    }
    
    return type;
}

// =============================================================================
// Sized Array Type: Array<T, N>
// =============================================================================

DependentType* create_sized_array_type(Type* element_type, int64_t size) {
    DependentType* type = dependent_type_new(DEPENDENT_SIZED_ARRAY, "Array");
    if (!type) return NULL;
    
    type->base_type = element_type;
    type->data.sized_array.element_type = element_type;
    type->data.sized_array.size = size;
    type->data.sized_array.is_size_dynamic = 0;
    
    // Add size constraint
    TypeConstraint* size_constraint = create_size_constraint(DEP_CONSTRAINT_SIZE_EQ, size);
    size_constraint->next = type->constraints;
    type->constraints = size_constraint;
    
    return type;
}

// =============================================================================
// Refinement Types
// =============================================================================

RefinementType* create_refinement_type(const char* name, Type* base_type, TypeConstraint* constraint) {
    RefinementType* type = xmalloc(sizeof(RefinementType));
    if (!type) return NULL;
    
    memset(type, 0, sizeof(RefinementType));
    type->name = strdup(name);
    type->base_type = base_type;
    type->constraint = constraint;
    
    return type;
}

void refinement_type_free(RefinementType* type) {
    if (!type) return;
    
    free(type->name);
    if (type->constraint) {
        type_constraint_free(type->constraint);
    }
    free(type);
}

RefinementType* create_non_zero_int_type(void) {
    TypeConstraint* constraint = create_non_zero_constraint();
    return create_refinement_type("NonZeroInt", NULL, constraint);
}

RefinementType* create_positive_int_type(void) {
    TypeConstraint* constraint = create_positive_constraint();
    return create_refinement_type("PositiveInt", NULL, constraint);
}

RefinementType* create_negative_int_type(void) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_NEGATIVE);
    return create_refinement_type("NegativeInt", NULL, constraint);
}

RefinementType* create_even_int_type(void) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_EVEN);
    return create_refinement_type("EvenInt", NULL, constraint);
}

RefinementType* create_valid_index_type(const char* array_name) {
    TypeConstraint* constraint = create_valid_index_constraint(array_name);
    return create_refinement_type("ValidIndex", NULL, constraint);
}

// =============================================================================
// Type Constraint System
// =============================================================================

TypeConstraint* type_constraint_new(DependentConstraintType type) {
    TypeConstraint* constraint = xmalloc(sizeof(TypeConstraint));
    if (!constraint) return NULL;
    
    memset(constraint, 0, sizeof(TypeConstraint));
    constraint->type = type;
    
    return constraint;
}

void type_constraint_free(TypeConstraint* constraint) {
    if (!constraint) return;
    
    free(constraint->name);
    if (constraint->expression) {
        symbolic_expression_free(constraint->expression);
    }
    
    switch (constraint->type) {
        case DEP_CONSTRAINT_VALID_INDEX:
            free(constraint->data.valid_index.target_array);
            break;
        default:
            // No additional cleanup needed
            break;
    }
    
    free(constraint);
}

TypeConstraint* create_range_constraint(int64_t min_value, int64_t max_value) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_RANGE);
    if (constraint) {
        constraint->data.range.min_value = min_value;
        constraint->data.range.max_value = max_value;
        constraint->name = malloc(64);
        if (constraint->name) {
            snprintf(constraint->name, 64, "Range[%lld, %lld]", 
                    (long long)min_value, (long long)max_value);
        }
    }
    return constraint;
}

TypeConstraint* create_non_zero_constraint(void) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_NON_ZERO);
    if (constraint) {
        constraint->name = strdup("NonZero");
    }
    return constraint;
}

TypeConstraint* create_positive_constraint(void) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_POSITIVE);
    if (constraint) {
        constraint->name = strdup("Positive");
    }
    return constraint;
}

TypeConstraint* create_size_constraint(DependentConstraintType size_type, int64_t size) {
    TypeConstraint* constraint = type_constraint_new(size_type);
    if (constraint) {
        constraint->data.size.size = size;
        constraint->name = malloc(64);
        if (constraint->name) {
            const char* op = "";
            switch (size_type) {
                case DEP_CONSTRAINT_SIZE_EQ: op = "=="; break;
                case DEP_CONSTRAINT_SIZE_LE: op = "<="; break;
                case DEP_CONSTRAINT_SIZE_GE: op = ">="; break;
                default: op = "?"; break;
            }
            snprintf(constraint->name, 64, "Size%s%lld", op, (long long)size);
        }
    }
    return constraint;
}

TypeConstraint* create_valid_index_constraint(const char* array_name) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_VALID_INDEX);
    if (constraint) {
        constraint->data.valid_index.target_array = strdup(array_name);
        constraint->name = malloc(64);
        if (constraint->name) {
            snprintf(constraint->name, 64, "ValidIndex<%s>", array_name);
        }
    }
    return constraint;
}

TypeConstraint* create_custom_constraint(SymbolicExpression* expression) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_CUSTOM);
    if (constraint) {
        constraint->expression = expression;
        constraint->name = strdup("Custom");
    }
    return constraint;
}

// =============================================================================
// Type Parameter System
// =============================================================================

TypeParameter* type_parameter_new(TypeParameterKind kind, const char* name) {
    TypeParameter* param = xmalloc(sizeof(TypeParameter));
    if (!param) return NULL;
    
    memset(param, 0, sizeof(TypeParameter));
    param->kind = kind;
    param->name = strdup(name);
    
    return param;
}

void type_parameter_free(TypeParameter* param) {
    if (!param) return;
    
    free(param->name);
    
    switch (param->kind) {
        case TYPE_PARAM_TYPE:
            if (param->data.type_param.bounds) {
                free(param->data.type_param.bounds);
            }
            break;
        case TYPE_PARAM_CONSTRAINT:
            if (param->data.constraint_param.constraint) {
                type_constraint_free(param->data.constraint_param.constraint);
            }
            break;
        default:
            break;
    }
    
    free(param);
}

int bind_type_parameter(DependentTypeContext* context, TypeParameter* param, Type* type) {
    if (!context || !param || !type || param->kind != TYPE_PARAM_TYPE) {
        return 0;
    }
    
    // For now, just store the binding - would implement proper type environment
    param->base_type = type;
    return 1;
}

int bind_value_parameter(DependentTypeContext* context, TypeParameter* param, int64_t value) {
    if (!context || !param || param->kind != TYPE_PARAM_VALUE) {
        return 0;
    }
    
    param->data.value_param.value = value;
    param->data.value_param.is_resolved = 1;
    return 1;
}

// =============================================================================
// Constraint Solver
// =============================================================================

ConstraintSolver* constraint_solver_new(DependentTypeContext* context) {
    ConstraintSolver* solver = xmalloc(sizeof(ConstraintSolver));
    if (!solver) return NULL;
    
    memset(solver, 0, sizeof(ConstraintSolver));
    solver->context = context;
    
    // Set default configuration
    solver->enable_smt_backend = 0;        // Disabled for now
    solver->enable_interval_analysis = 1;
    solver->enable_symbolic_execution = 1;
    solver->timeout_seconds = 5.0;
    
    // Initialize constraint database
    solver->known_constraint_capacity = 64;
    solver->known_constraints = malloc(sizeof(TypeConstraint*) * solver->known_constraint_capacity);
    if (solver->known_constraints) {
        memset(solver->known_constraints, 0, sizeof(TypeConstraint*) * solver->known_constraint_capacity);
    }
    
    return solver;
}

void constraint_solver_free(ConstraintSolver* solver) {
    if (!solver) return;
    
    if (solver->known_constraints) {
        for (size_t i = 0; i < solver->known_constraint_count; i++) {
            if (solver->known_constraints[i]) {
                type_constraint_free(solver->known_constraints[i]);
            }
        }
        free(solver->known_constraints);
    }
    
    free(solver);
}

SolverResult solve_constraint(ConstraintSolver* solver, TypeConstraint* constraint,
                             Type* type, ASTNode* expr) {
    if (!solver || !constraint) {
        return SOLVER_RESULT_ERROR;
    }
    
    solver->queries_solved++;
    
    // Simple constraint solving based on constraint type
    switch (constraint->type) {
        case DEP_CONSTRAINT_NON_ZERO:
            // Check if expression is obviously non-zero
            if (expr && expr->type == AST_LITERAL) {
                LiteralNode* lit = (LiteralNode*)expr;
                if (lit->literal_type == TOKEN_INT) {
                    int64_t value = strtoll(lit->value, NULL, 10);
                    return (value != 0) ? SOLVER_RESULT_SATISFIED : SOLVER_RESULT_UNSATISFIED;
                }
            }
            return SOLVER_RESULT_UNKNOWN;
            
        case DEP_CONSTRAINT_POSITIVE:
            if (expr && expr->type == AST_LITERAL) {
                LiteralNode* lit = (LiteralNode*)expr;
                if (lit->literal_type == TOKEN_INT) {
                    int64_t value = strtoll(lit->value, NULL, 10);
                    return (value > 0) ? SOLVER_RESULT_SATISFIED : SOLVER_RESULT_UNSATISFIED;
                }
            }
            return SOLVER_RESULT_UNKNOWN;
            
        case DEP_CONSTRAINT_RANGE:
            if (expr && expr->type == AST_LITERAL) {
                LiteralNode* lit = (LiteralNode*)expr;
                if (lit->literal_type == TOKEN_INT) {
                    int64_t value = strtoll(lit->value, NULL, 10);
                    int64_t min = constraint->data.range.min_value;
                    int64_t max = constraint->data.range.max_value;
                    return (value >= min && value <= max) ? 
                           SOLVER_RESULT_SATISFIED : SOLVER_RESULT_UNSATISFIED;
                }
            }
            return SOLVER_RESULT_UNKNOWN;
            
        case DEP_CONSTRAINT_EVEN:
            if (expr && expr->type == AST_LITERAL) {
                LiteralNode* lit = (LiteralNode*)expr;
                if (lit->literal_type == TOKEN_INT) {
                    int64_t value = strtoll(lit->value, NULL, 10);
                    return (value % 2 == 0) ? SOLVER_RESULT_SATISFIED : SOLVER_RESULT_UNSATISFIED;
                }
            }
            return SOLVER_RESULT_UNKNOWN;
            
        default:
            return SOLVER_RESULT_UNKNOWN;
    }
}

SolverResult solve_constraint_set(ConstraintSolver* solver, TypeConstraint** constraints,
                                 size_t constraint_count, Type** types, ASTNode** exprs) {
    if (!solver || !constraints || constraint_count == 0) {
        return SOLVER_RESULT_ERROR;
    }
    
    // Check each constraint individually
    for (size_t i = 0; i < constraint_count; i++) {
        Type* type = types ? types[i] : NULL;
        ASTNode* expr = exprs ? exprs[i] : NULL;
        
        SolverResult result = solve_constraint(solver, constraints[i], type, expr);
        if (result == SOLVER_RESULT_UNSATISFIED || result == SOLVER_RESULT_ERROR) {
            return result;
        }
    }
    
    return SOLVER_RESULT_SATISFIED;
}

// =============================================================================
// Type Checking Integration
// =============================================================================

int check_type_constraint(DependentTypeContext* context, TypeConstraint* constraint, 
                         Type* type, ASTNode* value_expr) {
    if (!context || !constraint) return 0;
    
    SolverResult result = solve_constraint(context->solver, constraint, type, value_expr);
    
    switch (result) {
        case SOLVER_RESULT_SATISFIED:
            context->constraints_verified++;
            return 1;
        case SOLVER_RESULT_UNSATISFIED:
            context->constraints_failed++;
            return 0;
        default:
            // Unknown result - be conservative
            if (context->strict_constraint_checking) {
                context->constraints_failed++;
                return 0;
            } else {
                // Allow with warning
                return 1;
            }
    }
}

int verify_refinement_constraints(DependentTypeContext* context, RefinementType* refinement,
                                 Type* actual_type, ASTNode* value_expr) {
    if (!context || !refinement || !refinement->constraint) return 0;
    
    return check_type_constraint(context, refinement->constraint, actual_type, value_expr);
}

Type* instantiate_dependent_type(DependentTypeContext* context, DependentType* dep_type,
                                TypeParameter* args) {
    if (!context || !dep_type) return NULL;
    
    context->type_instantiations++;
    
    // For now, return the base type
    // Real implementation would create specialized type instances
    return dep_type->base_type;
}

// =============================================================================
// Built-in Type Registration
// =============================================================================

void register_builtin_dependent_types(DependentTypeContext* context) {
    if (!context) return;
    
    // Create some common bounded integer types
    DependentType* uint8_type = create_bounded_int_type(0, 255);
    DependentType* uint16_type = create_bounded_int_type(0, 65535);
    DependentType* int8_type = create_bounded_int_type(-128, 127);
    
    // Store in context (simplified)
    if (context->dependent_type_count < context->dependent_type_capacity) {
        context->dependent_types[context->dependent_type_count++] = uint8_type;
    }
    if (context->dependent_type_count < context->dependent_type_capacity) {
        context->dependent_types[context->dependent_type_count++] = uint16_type;
    }
    if (context->dependent_type_count < context->dependent_type_capacity) {
        context->dependent_types[context->dependent_type_count++] = int8_type;
    }
}

void register_builtin_refinement_types(DependentTypeContext* context) {
    if (!context) return;
    
    // Create common refinement types
    RefinementType* non_zero = create_non_zero_int_type();
    RefinementType* positive = create_positive_int_type();
    RefinementType* negative = create_negative_int_type();
    RefinementType* even = create_even_int_type();
    
    // Add to context's refinement type list
    non_zero->next = context->refinement_types;
    context->refinement_types = non_zero;
    
    positive->next = context->refinement_types;
    context->refinement_types = positive;
    
    negative->next = context->refinement_types;
    context->refinement_types = negative;
    
    even->next = context->refinement_types;
    context->refinement_types = even;
}

DependentType* lookup_dependent_type(DependentTypeContext* context, const char* name) {
    if (!context || !name) return NULL;
    
    for (size_t i = 0; i < context->dependent_type_count; i++) {
        DependentType* type = context->dependent_types[i];
        if (type && type->name && strcmp(type->name, name) == 0) {
            return type;
        }
    }
    
    return NULL;
}

RefinementType* lookup_refinement_type(DependentTypeContext* context, const char* name) {
    if (!context || !name) return NULL;
    
    RefinementType* current = context->refinement_types;
    while (current) {
        if (current->name && strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

// =============================================================================
// Utility Functions
// =============================================================================

char* type_constraint_to_string(const TypeConstraint* constraint) {
    if (!constraint) return strdup("null");
    
    char* result = malloc(256);
    if (!result) return NULL;
    
    switch (constraint->type) {
        case DEP_CONSTRAINT_RANGE:
            snprintf(result, 256, "Range[%lld, %lld]", 
                    (long long)constraint->data.range.min_value,
                    (long long)constraint->data.range.max_value);
            break;
        case DEP_CONSTRAINT_NON_ZERO:
            snprintf(result, 256, "NonZero");
            break;
        case DEP_CONSTRAINT_POSITIVE:
            snprintf(result, 256, "Positive");
            break;
        case DEP_CONSTRAINT_NEGATIVE:
            snprintf(result, 256, "Negative");
            break;
        case DEP_CONSTRAINT_EVEN:
            snprintf(result, 256, "Even");
            break;
        case DEP_CONSTRAINT_ODD:
            snprintf(result, 256, "Odd");
            break;
        case DEP_CONSTRAINT_SIZE_EQ:
            snprintf(result, 256, "Size == %lld", (long long)constraint->data.size.size);
            break;
        case DEP_CONSTRAINT_SIZE_LE:
            snprintf(result, 256, "Size <= %lld", (long long)constraint->data.size.size);
            break;
        case DEP_CONSTRAINT_SIZE_GE:
            snprintf(result, 256, "Size >= %lld", (long long)constraint->data.size.size);
            break;
        case DEP_CONSTRAINT_VALID_INDEX:
            snprintf(result, 256, "ValidIndex<%s>", 
                    constraint->data.valid_index.target_array ? 
                    constraint->data.valid_index.target_array : "?");
            break;
        default:
            snprintf(result, 256, "Unknown");
            break;
    }
    
    return result;
}

char* dependent_type_to_string(const DependentType* type) {
    if (!type) return strdup("null");
    
    char* result = malloc(512);
    if (!result) return NULL;
    
    switch (type->kind) {
        case DEPENDENT_BOUNDED_VEC:
            if (type->data.bounded_vec.is_capacity_dynamic) {
                snprintf(result, 512, "BoundedVec<T, %s>", 
                        type->data.bounded_vec.capacity_param);
            } else {
                snprintf(result, 512, "BoundedVec<T, %lld>", 
                        (long long)type->data.bounded_vec.capacity);
            }
            break;
        case DEPENDENT_BOUNDED_INT:
            if (type->data.bounded_int.is_min_dynamic || type->data.bounded_int.is_max_dynamic) {
                snprintf(result, 512, "BoundedInt<%s, %s>",
                        type->data.bounded_int.min_param ? type->data.bounded_int.min_param : "?",
                        type->data.bounded_int.max_param ? type->data.bounded_int.max_param : "?");
            } else {
                snprintf(result, 512, "BoundedInt<%lld, %lld>",
                        (long long)type->data.bounded_int.min_value,
                        (long long)type->data.bounded_int.max_value);
            }
            break;
        case DEPENDENT_SIZED_ARRAY:
            if (type->data.sized_array.is_size_dynamic) {
                snprintf(result, 512, "Array<T, %s>", type->data.sized_array.size_param);
            } else {
                snprintf(result, 512, "Array<T, %lld>", 
                        (long long)type->data.sized_array.size);
            }
            break;
        default:
            snprintf(result, 512, "%s", type->name ? type->name : "Unknown");
            break;
    }
    
    return result;
}

const char* dependent_type_kind_to_string(DependentTypeKind kind) {
    switch (kind) {
        case DEPENDENT_BOUNDED_VEC: return "BoundedVec";
        case DEPENDENT_BOUNDED_INT: return "BoundedInt";
        case DEPENDENT_SIZED_ARRAY: return "SizedArray";
        case DEPENDENT_REFINED_TYPE: return "RefinedType";
        case DEPENDENT_INDEXED_TYPE: return "IndexedType";
        case DEPENDENT_FUNCTION_TYPE: return "FunctionType";
        default: return "Unknown";
    }
}

const char* dependent_constraint_type_to_string(DependentConstraintType type) {
    switch (type) {
        case DEP_CONSTRAINT_RANGE: return "Range";
        case DEP_CONSTRAINT_NON_ZERO: return "NonZero";
        case DEP_CONSTRAINT_POSITIVE: return "Positive";
        case DEP_CONSTRAINT_NEGATIVE: return "Negative";
        case DEP_CONSTRAINT_EVEN: return "Even";
        case DEP_CONSTRAINT_ODD: return "Odd";
        case DEP_CONSTRAINT_SIZE_EQ: return "SizeEq";
        case DEP_CONSTRAINT_SIZE_LE: return "SizeLe";
        case DEP_CONSTRAINT_SIZE_GE: return "SizeGe";
        case DEP_CONSTRAINT_VALID_INDEX: return "ValidIndex";
        case DEP_CONSTRAINT_DIVISIBLE: return "Divisible";
        case DEP_CONSTRAINT_CUSTOM: return "Custom";
        default: return "Unknown";
    }
}

const char* solver_result_to_string(SolverResult result) {
    switch (result) {
        case SOLVER_RESULT_SATISFIED: return "Satisfied";
        case SOLVER_RESULT_UNSATISFIED: return "Unsatisfied";
        case SOLVER_RESULT_UNKNOWN: return "Unknown";
        case SOLVER_RESULT_TIMEOUT: return "Timeout";
        case SOLVER_RESULT_ERROR: return "Error";
        default: return "Invalid";
    }
}

void dependent_type_context_print_statistics(DependentTypeContext* context) {
    if (!context) return;
    
    printf("=== Dependent Type System Statistics ===\n");
    printf("Types checked: %zu\n", context->types_checked);
    printf("Constraints verified: %zu\n", context->constraints_verified);
    printf("Constraints failed: %zu\n", context->constraints_failed);
    printf("Type instantiations: %zu\n", context->type_instantiations);
    printf("Dependent types registered: %zu\n", context->dependent_type_count);
    
    // Count refinement types
    size_t refinement_count = 0;
    RefinementType* current = context->refinement_types;
    while (current) {
        refinement_count++;
        current = current->next;
    }
    printf("Refinement types registered: %zu\n", refinement_count);
    
    printf("\nConfiguration:\n");
    printf("Dependent types: %s\n", context->enable_dependent_types ? "enabled" : "disabled");
    printf("Refinement types: %s\n", context->enable_refinement_types ? "enabled" : "disabled");
    printf("Constraint inference: %s\n", context->enable_constraint_inference ? "enabled" : "disabled");
    printf("Strict checking: %s\n", context->strict_constraint_checking ? "enabled" : "disabled");
}

void constraint_solver_print_statistics(ConstraintSolver* solver) {
    if (!solver) return;
    
    printf("=== Constraint Solver Statistics ===\n");
    printf("Queries solved: %zu\n", solver->queries_solved);
    printf("Queries timeout: %zu\n", solver->queries_timeout);
    printf("Queries failed: %zu\n", solver->queries_failed);
    printf("Total solve time: %.3f seconds\n", solver->total_solve_time);
    printf("Known constraints: %zu\n", solver->known_constraint_count);
    
    printf("\nConfiguration:\n");
    printf("SMT backend: %s\n", solver->enable_smt_backend ? "enabled" : "disabled");
    printf("Interval analysis: %s\n", solver->enable_interval_analysis ? "enabled" : "disabled");
    printf("Symbolic execution: %s\n", solver->enable_symbolic_execution ? "enabled" : "disabled");
    printf("Timeout: %.1f seconds\n", solver->timeout_seconds);
}

void dependent_type_context_enable_feature(DependentTypeContext* context, 
                                          const char* feature, int enable) {
    if (!context || !feature) return;
    
    if (strcmp(feature, "dependent_types") == 0) {
        context->enable_dependent_types = enable;
    } else if (strcmp(feature, "refinement_types") == 0) {
        context->enable_refinement_types = enable;
    } else if (strcmp(feature, "constraint_inference") == 0) {
        context->enable_constraint_inference = enable;
    } else if (strcmp(feature, "strict_checking") == 0) {
        context->strict_constraint_checking = enable;
    }
}

// Removed: symbolic_expression_free - properly implemented in bounds_verifier.c
// // Stub function for symbolic expression free
// void symbolic_expression_free(SymbolicExpression* expr) {
//     // This would free symbolic expression structures
//     // For now, just a stub
//     if (expr) {
//         free(expr);
//     }
// }