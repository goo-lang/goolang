#include "types.h"
#include "comptime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Helper function to duplicate strings
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// Type creation functions

Type* type_new(TypeKind kind) {
    Type* type = malloc(sizeof(Type));
    if (!type) return NULL;
    
    memset(type, 0, sizeof(Type));
    type->kind = kind;
    type->name = NULL;
    
    return type;
}

Type* type_void(void) {
    Type* type = type_new(TYPE_VOID);
    if (type) {
        type->size = 0;
        type->align = 1;
        type->name = str_dup("void");
    }
    return type;
}

Type* type_bool(void) {
    Type* type = type_new(TYPE_BOOL);
    if (type) {
        type->size = 1;
        type->align = 1;
        type->name = str_dup("bool");
    }
    return type;
}

Type* type_int(int bits, int is_signed) {
    TypeKind kind;
    const char* name;
    
    switch (bits) {
        case 8:
            kind = is_signed ? TYPE_INT8 : TYPE_UINT8;
            name = is_signed ? "int8" : "uint8";
            break;
        case 16:
            kind = is_signed ? TYPE_INT16 : TYPE_UINT16;
            name = is_signed ? "int16" : "uint16";
            break;
        case 32:
            kind = is_signed ? TYPE_INT32 : TYPE_UINT32;
            name = is_signed ? "int32" : "uint32";
            break;
        case 64:
            kind = is_signed ? TYPE_INT64 : TYPE_UINT64;
            name = is_signed ? "int64" : "uint64";
            break;
        default:
            return NULL;
    }
    
    Type* type = type_new(kind);
    if (type) {
        type->size = bits / 8;
        type->align = bits / 8;
        type->name = str_dup(name);
    }
    return type;
}

Type* type_float(int bits) {
    TypeKind kind;
    const char* name;
    
    switch (bits) {
        case 32:
            kind = TYPE_FLOAT32;
            name = "float32";
            break;
        case 64:
            kind = TYPE_FLOAT64;
            name = "float64";
            break;
        default:
            return NULL;
    }
    
    Type* type = type_new(kind);
    if (type) {
        type->size = bits / 8;
        type->align = bits / 8;
        type->name = str_dup(name);
    }
    return type;
}

Type* type_string_type(void) {
    Type* type = type_new(TYPE_STRING);
    if (type) {
        type->size = sizeof(void*);  // String is a pointer to data
        type->align = sizeof(void*);
        type->name = str_dup("string");
    }
    return type;
}

Type* type_char(void) {
    Type* type = type_new(TYPE_CHAR);
    if (type) {
        type->size = 1;
        type->align = 1;
        type->name = str_dup("char");
    }
    return type;
}

Type* type_array(Type* element_type, size_t length) {
    if (!element_type) return NULL;
    
    Type* type = type_new(TYPE_ARRAY);
    if (type) {
        type->data.array.element_type = element_type;
        type->data.array.length = length;
        type->size = element_type->size * length;
        type->align = element_type->align;
        
        // Create name like "[10]int"
        char* name = malloc(64);
        if (name) {
            snprintf(name, 64, "[%zu]%s", length, element_type->name ? element_type->name : "?");
            type->name = name;
        }
    }
    return type;
}

Type* type_slice(Type* element_type) {
    if (!element_type) return NULL;
    
    Type* type = type_new(TYPE_SLICE);
    if (type) {
        type->data.slice.element_type = element_type;
        type->size = sizeof(void*) + sizeof(size_t) + sizeof(size_t);  // ptr + len + cap
        type->align = sizeof(void*);
        
        // Create name like "[]int"
        char* name = malloc(64);
        if (name) {
            snprintf(name, 64, "[]%s", element_type->name ? element_type->name : "?");
            type->name = name;
        }
    }
    return type;
}

Type* type_map(Type* key_type, Type* value_type) {
    if (!key_type || !value_type) return NULL;
    
    Type* type = type_new(TYPE_MAP);
    if (type) {
        type->data.map.key_type = key_type;
        type->data.map.value_type = value_type;
        type->size = sizeof(void*);  // Map is a pointer to internal structure
        type->align = sizeof(void*);
        
        // Create name like "map[string]int"
        char* name = malloc(128);
        if (name) {
            snprintf(name, 128, "map[%s]%s", 
                    key_type->name ? key_type->name : "?",
                    value_type->name ? value_type->name : "?");
            type->name = name;
        }
    }
    return type;
}

Type* type_channel(Type* element_type, ChannelPattern pattern) {
    if (!element_type) return NULL;
    
    Type* type = type_new(TYPE_CHANNEL);
    if (type) {
        type->data.channel.element_type = element_type;
        type->data.channel.pattern = pattern;
        type->data.channel.endpoint = NULL;
        type->size = sizeof(void*);  // Channel is a pointer
        type->align = sizeof(void*);
        
        // Create name like "chan int" or "pub chan Message"
        const char* pattern_str = "";
        switch (pattern) {
            case CHAN_PATTERN_PUB: pattern_str = "pub "; break;
            case CHAN_PATTERN_SUB: pattern_str = "sub "; break;
            case CHAN_PATTERN_REQ: pattern_str = "req "; break;
            case CHAN_PATTERN_REP: pattern_str = "rep "; break;
            case CHAN_PATTERN_PUSH: pattern_str = "push "; break;
            case CHAN_PATTERN_PULL: pattern_str = "pull "; break;
            default: break;
        }
        
        char* name = malloc(128);
        if (name) {
            snprintf(name, 128, "%schan %s", pattern_str,
                    element_type->name ? element_type->name : "?");
            type->name = name;
        }
    }
    return type;
}

Type* type_function(Type** param_types, size_t param_count, Type* return_type) {
    Type* type = type_new(TYPE_FUNCTION);
    if (type) {
        // Allocate and copy the parameter types array to avoid double-free issues
        if (param_count > 0 && param_types) {
            type->data.function.param_types = malloc(param_count * sizeof(Type*));
            if (!type->data.function.param_types) {
                free(type);
                return NULL;
            }
            memcpy(type->data.function.param_types, param_types, param_count * sizeof(Type*));
        } else {
            type->data.function.param_types = NULL;
        }
        type->data.function.param_count = param_count;
        type->data.function.return_type = return_type;
        type->data.function.is_variadic = 0;
        type->size = sizeof(void*);  // Function is a pointer
        type->align = sizeof(void*);

        // Create name like "func(int, int) int" (param type names comma-
        // joined; " " + return-type name suffix only when non-void) — same
        // exact-size-then-strcpy/strcat idiom as type_application's "Vec<int>"
        // above. Every TYPE_FUNCTION used to get the literal name "func", so
        // type_to_string (which just returns type->name) rendered every
        // signature identically — a func(int,int)int assigned to a
        // func(int)int var reported "Cannot assign func to func" instead of
        // naming the actual mismatch.
        int has_return = return_type && return_type->kind != TYPE_VOID;
        size_t name_size = strlen("func()") + 1;
        for (size_t i = 0; i < param_count; i++) {
            if (i > 0) name_size += 2;  // ", "
            name_size += strlen(param_types[i] && param_types[i]->name
                                 ? param_types[i]->name : "?");
        }
        if (has_return) {
            name_size += 1 + strlen(return_type->name ? return_type->name : "?");  // " " + name
        }
        char* name = malloc(name_size);
        if (name) {
            strcpy(name, "func(");
            for (size_t i = 0; i < param_count; i++) {
                if (i > 0) strcat(name, ", ");
                strcat(name, param_types[i] && param_types[i]->name
                             ? param_types[i]->name : "?");
            }
            strcat(name, ")");
            if (has_return) {
                strcat(name, " ");
                strcat(name, return_type->name ? return_type->name : "?");
            }
            type->name = name;
        } else {
            type->name = str_dup("func");  // OOM fallback
        }
    }
    return type;
}

Type* type_pointer(Type* pointee_type) {
    if (!pointee_type) return NULL;
    
    Type* type = type_new(TYPE_POINTER);
    if (type) {
        type->data.pointer.pointee_type = pointee_type;
        type->size = sizeof(void*);
        type->align = sizeof(void*);
        
        // Create name like "*int"
        char* name = malloc(64);
        if (name) {
            snprintf(name, 64, "*%s", pointee_type->name ? pointee_type->name : "?");
            type->name = name;
        }
    }
    return type;
}

Type* type_reference(Type* referenced_type, int is_mutable) {
    if (!referenced_type) return NULL;
    
    Type* type = type_new(TYPE_REFERENCE);
    if (type) {
        type->data.reference.referenced_type = referenced_type;
        type->data.reference.is_mutable = is_mutable;
        type->size = sizeof(void*);
        type->align = sizeof(void*);
        
        // Create name like "&int" or "mut &int"
        char* name = malloc(80);
        if (name) {
            if (is_mutable) {
                snprintf(name, 80, "mut &%s", referenced_type->name ? referenced_type->name : "?");
            } else {
                snprintf(name, 80, "&%s", referenced_type->name ? referenced_type->name : "?");
            }
            type->name = name;
        }
    }
    return type;
}

// Goo extension types

Type* type_error_union(Type* value_type, Type* error_type) {
    if (!value_type) return NULL;
    
    Type* type = type_new(TYPE_ERROR_UNION);
    if (type) {
        type->data.error_union.value_type = value_type;
        type->data.error_union.error_type = error_type;  // Can be NULL for default error
        
        // Error union needs space for both value and error indicator
        type->size = value_type->size + sizeof(int);  // value + error flag
        type->align = (value_type->align > sizeof(int)) ? value_type->align : sizeof(int);
        
        // Create name like "!int"
        char* name = malloc(64);
        if (name) {
            snprintf(name, 64, "!%s", value_type->name ? value_type->name : "?");
            type->name = name;
        }
    }
    return type;
}

Type* type_nullable(Type* base_type) {
    if (!base_type) return NULL;
    
    Type* type = type_new(TYPE_NULLABLE);
    if (type) {
        type->data.nullable.base_type = base_type;
        
        // Nullable type needs space for value + null indicator
        type->size = base_type->size + sizeof(char);  // value + null flag
        type->align = base_type->align;
        
        // Create name like "?int"
        char* name = malloc(64);
        if (name) {
            snprintf(name, 64, "?%s", base_type->name ? base_type->name : "?");
            type->name = name;
        }
    }
    return type;
}

Type* type_qualified(Type* base_type, OwnershipKind ownership, MutabilityKind mutability) {
    if (!base_type) return NULL;
    
    Type* type = type_new(TYPE_QUALIFIED);
    if (type) {
        type->data.qualified.base_type = base_type;
        type->data.qualified.ownership = ownership;
        type->data.qualified.mutability = mutability;
        type->size = base_type->size;
        type->align = base_type->align;
        
        // Create name with qualifiers
        const char* ownership_str = "";
        switch (ownership) {
            case OWNERSHIP_OWNED: ownership_str = "owned "; break;
            case OWNERSHIP_BORROWED: ownership_str = "borrowed "; break;
            case OWNERSHIP_SHARED: ownership_str = "shared "; break;
            default: break;
        }
        
        const char* mut_str = (mutability == MUTABILITY_MUTABLE) ? "mut " : "";
        
        char* name = malloc(128);
        if (name) {
            snprintf(name, 128, "%s%s%s", ownership_str, mut_str,
                    base_type->name ? base_type->name : "?");
            type->name = name;
        }
    }
    return type;
}

Type* type_concept(const char* name) {
    if (!name) return NULL;
    
    Type* type = type_new(TYPE_CONCEPT);
    if (type) {
        type->data.concept.name = str_dup(name);
        type->data.concept.type_params = NULL;
        type->data.concept.requirements = NULL;
        type->size = 0;  // Concepts are compile-time only
        type->align = 0;
        type->name = str_dup(name);
    }
    return type;
}

// Higher-kinded type creation functions

Type* type_param(const char* name, int index, Type* constraint) {
    if (!name) return NULL;
    
    Type* type = type_new(TYPE_PARAM);
    if (type) {
        type->data.type_param.name = str_dup(name);
        type->data.type_param.index = index;
        type->data.type_param.constraint = constraint;
        type->size = 0;  // Type parameters are compile-time only
        type->align = 0;
        type->name = str_dup(name);
    }
    return type;
}

Type* type_param_hkt(const char* name, int index, size_t arity, const char* kind_signature, Type* constraint) {
    if (!name) return NULL;
    
    Type* type = type_new(TYPE_PARAM_HKT);
    if (type) {
        type->data.hkt_param.name = str_dup(name);
        type->data.hkt_param.index = index;
        type->data.hkt_param.arity = arity;
        type->data.hkt_param.kind_signature = str_dup(kind_signature ? kind_signature : "* -> *");
        type->data.hkt_param.constraint = constraint;
        type->size = 0;  // Type parameters are compile-time only
        type->align = 0;
        
        // Create name like "F<_>" for HKT parameters
        char* display_name = malloc(strlen(name) + 10);
        if (display_name) {
            snprintf(display_name, strlen(name) + 10, "%s<_>", name);
            type->name = display_name;
        } else {
            type->name = str_dup(name);
        }
    }
    return type;
}

Type* type_constructor(const char* name, size_t arity, const char* kind_signature) {
    if (!name) return NULL;
    
    Type* type = type_new(TYPE_CONSTRUCTOR);
    if (type) {
        type->data.constructor.name = str_dup(name);
        type->data.constructor.arity = arity;
        type->data.constructor.params = NULL;
        type->data.constructor.param_count = 0;
        type->data.constructor.kind_signature = str_dup(kind_signature ? kind_signature : "*");
        type->size = 0;  // Type constructors are compile-time only
        type->align = 0;
        type->name = str_dup(name);
    }
    return type;
}

Type* type_application(Type* constructor, Type** arguments, size_t arg_count) {
    if (!constructor || !arguments || arg_count == 0) return NULL;
    
    Type* type = type_new(TYPE_APPLICATION);
    if (type) {
        type->data.application.constructor = constructor;
        type->data.application.arguments = malloc(sizeof(Type*) * arg_count);
        if (type->data.application.arguments) {
            memcpy(type->data.application.arguments, arguments, sizeof(Type*) * arg_count);
            type->data.application.arg_count = arg_count;
        }
        
        // Size and alignment depend on the fully applied type
        // This will be resolved during type checking
        type->size = 0;
        type->align = 0;
        
        // Create name like "Vec<int>"
        if (constructor->name && arg_count > 0) {
            size_t name_size = strlen(constructor->name) + 3;  // For "<>"
            for (size_t i = 0; i < arg_count; i++) {
                if (arguments[i]->name) {
                    name_size += strlen(arguments[i]->name) + 2;  // For ", "
                }
            }
            
            char* name = malloc(name_size);
            if (name) {
                strcpy(name, constructor->name);
                strcat(name, "<");
                for (size_t i = 0; i < arg_count; i++) {
                    if (i > 0) strcat(name, ", ");
                    strcat(name, arguments[i]->name ? arguments[i]->name : "?");
                }
                strcat(name, ">");
                type->name = name;
            }
        }
    }
    return type;
}

// Type operations

void type_free(Type* type) {
    if (!type) return;
    
    free(type->name);
    
    // Free type-specific data
    switch (type->kind) {
        case TYPE_FUNCTION:
            if (type->data.function.param_types) {
                free(type->data.function.param_types);
            }
            break;
        case TYPE_STRUCT:
            for (size_t i = 0; i < type->data.struct_type.field_count; i++) {
                free(type->data.struct_type.fields[i].name);
            }
            free(type->data.struct_type.fields);
            free(type->data.struct_type.name);
            break;
        case TYPE_INTERFACE:
            for (size_t i = 0; i < type->data.interface.method_count; i++) {
                free(type->data.interface.methods[i].name);
            }
            free(type->data.interface.methods);
            free(type->data.interface.name);
            break;
        case TYPE_CHANNEL:
            free(type->data.channel.endpoint);
            break;
        case TYPE_CONCEPT:
            free(type->data.concept.name);
            // Note: type_params and requirements are AST nodes,
            // they should be freed by the AST cleanup, not here
            break;
        case TYPE_PARAM:
            free(type->data.type_param.name);
            // Note: constraint is a Type*, should be freed separately if owned
            break;
        case TYPE_PARAM_HKT:
            free(type->data.hkt_param.name);
            free(type->data.hkt_param.kind_signature);
            // Note: constraint is a Type*, should be freed separately if owned
            break;
        case TYPE_CONSTRUCTOR:
            free(type->data.constructor.name);
            free(type->data.constructor.kind_signature);
            if (type->data.constructor.params) {
                free(type->data.constructor.params);
            }
            break;
        case TYPE_APPLICATION:
            // Note: constructor and arguments are Type*, should be freed separately if owned
            if (type->data.application.arguments) {
                free(type->data.application.arguments);
            }
            break;
        default:
            break;
    }
    
    free(type);
}

Type* type_copy(const Type* type) {
    if (!type) return NULL;
    
    Type* copy = malloc(sizeof(Type));
    if (!copy) return NULL;
    
    *copy = *type;
    copy->name = str_dup(type->name);
    
    // Deep copy type-specific data as needed
    // TODO: Implement deep copying for complex types
    
    return copy;
}

int type_equals(const Type* a, const Type* b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return 0;
    
    switch (a->kind) {
        case TYPE_ARRAY:
            return a->data.array.length == b->data.array.length &&
                   type_equals(a->data.array.element_type, b->data.array.element_type);
        
        case TYPE_SLICE:
            return type_equals(a->data.slice.element_type, b->data.slice.element_type);
        
        case TYPE_MAP:
            return type_equals(a->data.map.key_type, b->data.map.key_type) &&
                   type_equals(a->data.map.value_type, b->data.map.value_type);
        
        case TYPE_CHANNEL:
            return a->data.channel.pattern == b->data.channel.pattern &&
                   type_equals(a->data.channel.element_type, b->data.channel.element_type);
        
        case TYPE_POINTER:
            return type_equals(a->data.pointer.pointee_type, b->data.pointer.pointee_type);
        
        case TYPE_REFERENCE:
            return a->data.reference.is_mutable == b->data.reference.is_mutable &&
                   type_equals(a->data.reference.referenced_type, b->data.reference.referenced_type);
        
        case TYPE_ERROR_UNION:
            return type_equals(a->data.error_union.value_type, b->data.error_union.value_type) &&
                   type_equals(a->data.error_union.error_type, b->data.error_union.error_type);
        
        case TYPE_NULLABLE:
            return type_equals(a->data.nullable.base_type, b->data.nullable.base_type);
        
        case TYPE_QUALIFIED:
            return a->data.qualified.ownership == b->data.qualified.ownership &&
                   a->data.qualified.mutability == b->data.qualified.mutability &&
                   type_equals(a->data.qualified.base_type, b->data.qualified.base_type);
        
        case TYPE_PARAM:
            return strcmp(a->data.type_param.name, b->data.type_param.name) == 0 &&
                   a->data.type_param.index == b->data.type_param.index;
        
        case TYPE_PARAM_HKT:
            return strcmp(a->data.hkt_param.name, b->data.hkt_param.name) == 0 &&
                   a->data.hkt_param.index == b->data.hkt_param.index &&
                   a->data.hkt_param.arity == b->data.hkt_param.arity;
        
        case TYPE_CONSTRUCTOR:
            return strcmp(a->data.constructor.name, b->data.constructor.name) == 0 &&
                   a->data.constructor.arity == b->data.constructor.arity;
        
        case TYPE_APPLICATION:
            if (!type_equals(a->data.application.constructor, b->data.application.constructor))
                return 0;
            if (a->data.application.arg_count != b->data.application.arg_count)
                return 0;
            for (size_t i = 0; i < a->data.application.arg_count; i++) {
                if (!type_equals(a->data.application.arguments[i], b->data.application.arguments[i]))
                    return 0;
            }
            return 1;

        case TYPE_FUNCTION:
            // Signature equality: same arity, each param type, return type,
            // and variadic flag. Without this arm the switch's default (any
            // two same-kind types are equal) made every func value type
            // compatible with every other, e.g. `func(int) int` accepted
            // where `func(string) bool` was expected.
            if (a->data.function.is_variadic != b->data.function.is_variadic)
                return 0;
            if (a->data.function.param_count != b->data.function.param_count)
                return 0;
            for (size_t i = 0; i < a->data.function.param_count; i++) {
                if (!type_equals(a->data.function.param_types[i], b->data.function.param_types[i]))
                    return 0;
            }
            return type_equals(a->data.function.return_type, b->data.function.return_type);

        default:
            return 1;  // Basic types are equal if kinds match
    }
}

// Function generics Task 5: type_substitute and unify_types — the two
// standalone helpers generic-call inference (Task 6) builds on. Neither has a
// caller yet; both are exercised directly once Task 6 wires them into call
// checking. Deliberately Tier-A scoped: only TYPE_PARAM/SLICE/POINTER/FUNCTION
// recurse structurally (TYPE_ARRAY/TYPE_MAP are not — the Tier-A goldens only
// exercise slice/pointer/function/scalar generics), everything else is a
// concrete type handled via type_equals/identity.

// Returns a new Type* with every TYPE_PARAM of index i < n replaced by
// bindings[i], recursing through slice/pointer/function. A TYPE_PARAM with no
// binding (index out of range, or bindings[i] NULL) is returned as-is —
// callers see an un-substituted param rather than a silent NULL.
Type* type_substitute(Type* t, Type** bindings, size_t n) {
    if (!t) return NULL;
    switch (t->kind) {
        case TYPE_PARAM: {
            int i = t->data.type_param.index;
            if (i >= 0 && (size_t)i < n && bindings[i]) return bindings[i];
            return t;
        }
        case TYPE_SLICE:
            return type_slice(type_substitute(t->data.slice.element_type, bindings, n));
        case TYPE_POINTER:
            return type_pointer(type_substitute(t->data.pointer.pointee_type, bindings, n));
        case TYPE_FUNCTION: {
            size_t pc = t->data.function.param_count;
            Type** ps = pc ? calloc(pc, sizeof(Type*)) : NULL;
            for (size_t i = 0; i < pc; i++)
                ps[i] = type_substitute(t->data.function.param_types[i], bindings, n);
            Type* r = type_substitute(t->data.function.return_type, bindings, n);
            Type* ft = type_function(ps, pc, r);
            if (ft) ft->data.function.is_variadic = t->data.function.is_variadic;
            return ft;
        }
        default:
            return t; // concrete types are shared unchanged
    }
}

// Structurally matches param (may contain TYPE_PARAM) against concrete arg,
// writing inferred concrete types into bindings[index]. Returns 1 on success;
// 0 on a structural mismatch (differing kind/shape) or a conflicting binding
// (bindings[i] already set to a type that isn't type_equals to arg).
int unify_types(Type* param, Type* arg, Type** bindings, size_t n) {
    if (!param || !arg) return 0;
    if (param->kind == TYPE_PARAM) {
        int i = param->data.type_param.index;
        if (i < 0 || (size_t)i >= n) return 0;
        if (bindings[i]) return type_equals(bindings[i], arg);
        bindings[i] = arg;
        return 1;
    }
    if (param->kind != arg->kind) return 0;
    switch (param->kind) {
        case TYPE_SLICE:
            return unify_types(param->data.slice.element_type,
                               arg->data.slice.element_type, bindings, n);
        case TYPE_POINTER:
            return unify_types(param->data.pointer.pointee_type,
                               arg->data.pointer.pointee_type, bindings, n);
        case TYPE_FUNCTION: {
            if (param->data.function.param_count != arg->data.function.param_count)
                return 0;
            for (size_t i = 0; i < param->data.function.param_count; i++)
                if (!unify_types(param->data.function.param_types[i],
                                 arg->data.function.param_types[i], bindings, n))
                    return 0;
            return unify_types(param->data.function.return_type,
                               arg->data.function.return_type, bindings, n);
        }
        default:
            return type_equals(param, arg);
    }
}

int type_compatible(const Type* from, const Type* to) {
    if (type_equals(from, to)) return 1;
    
    // Handle implicit conversions
    if (type_is_numeric(from) && type_is_numeric(to)) {
        // Reject implicit FLOAT -> INT: codegen has no narrowing/truncation
        // path for this direction, so an accepted float->int assignment
        // silently bit-stores the float's raw bit pattern into the int slot
        // (`var i int64 = float32(2.5)` produced 1075838976, not 2) instead
        // of converting the value. int->float and float->float stay
        // permitted (PR #99 probes depend on int->float: `var y float64 =
        // x`, `[]float64{1, 2.5}`); an explicit conversion (`int64(g)`) is
        // still the way to get a float->int narrowing.
        if (type_is_float(from) && type_is_integer(to)) {
            return 0;
        }
        // Allow numeric conversions (with potential warnings)
        return 1;
    }
    
    // Handle nullable types
    if (to->kind == TYPE_NULLABLE) {
        // nil literal (TYPE_UNKNOWN) is assignable to any nullable type.
        if (from->kind == TYPE_UNKNOWN) return 1;
        return type_compatible(from, to->data.nullable.base_type);
    }
    
    // Handle ownership qualifiers
    if (from->kind == TYPE_QUALIFIED && to->kind == TYPE_QUALIFIED) {
        return type_compatible(from->data.qualified.base_type, to->data.qualified.base_type);
    }
    
    return 0;
}

const char* type_to_string(const Type* type) {
    if (!type) return "null";
    // Structs carry their declared name in data.struct_type.name, not the
    // shared `name` field (which is NULL for them) — so a bare `type->name`
    // rendered every struct as "(null)" in diagnostics (e.g. the map-key
    // reject messages). Prefer the struct name; fall back to "struct" for an
    // anonymous struct.
    if (type->kind == TYPE_STRUCT) {
        return type->data.struct_type.name ? type->data.struct_type.name : "struct";
    }
    return type->name ? type->name : "?";
}

size_t type_size(const Type* type) {
    return type ? type->size : 0;
}

size_t type_align(const Type* type) {
    return type ? type->align : 1;
}

// Type checking utilities

int type_is_integer(const Type* type) {
    if (!type) return 0;
    return type->kind >= TYPE_INT8 && type->kind <= TYPE_UINT64;
}

int type_is_float(const Type* type) {
    if (!type) return 0;
    return type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT64;
}

int type_is_numeric(const Type* type) {
    return type_is_integer(type) || type_is_float(type);
}

int type_is_signed(const Type* type) {
    if (!type) return 0;
    return type->kind == TYPE_INT8 || type->kind == TYPE_INT16 ||
           type->kind == TYPE_INT32 || type->kind == TYPE_INT64 ||
           type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT64;
}

int type_is_pointer_like(const Type* type) {
    if (!type) return 0;
    return type->kind == TYPE_POINTER || type->kind == TYPE_SLICE ||
           type->kind == TYPE_MAP || type->kind == TYPE_CHANNEL ||
           type->kind == TYPE_STRING;
}

int type_is_nullable(const Type* type) {
    if (!type) return 0;
    return type->kind == TYPE_NULLABLE;
}

int type_is_error_union(const Type* type) {
    if (!type) return 0;
    return type->kind == TYPE_ERROR_UNION;
}

// A Goo v1 `error` value is the nullable pointer tagged name=="error" by
// type_checker_error_type(). Central predicate so .Error() dispatch and fmt
// error-printing recognize errors identically.
int type_is_error(const Type* t) {
    return t && t->name && strcmp(t->name, "error") == 0;
}

// Method name mangling: `func (T) m()` is lowered to an ordinary function
// named "T__m". The declaration and every call site derive the same name
// from the receiver's type, so a plain function/variable lookup resolves
// methods without a separate method table. Returns a malloc'd string.
char* type_method_mangled_name(const char* type_name, const char* method_name) {
    if (!type_name || !method_name) return NULL;
    size_t n = strlen(type_name) + 2 /* "__" */ + strlen(method_name) + 1;
    char* buf = malloc(n);
    if (!buf) return NULL;
    snprintf(buf, n, "%s__%s", type_name, method_name);
    return buf;
}

// Receiver type name for mangling: unwraps a pointer receiver (*T) to T and
// returns the struct's declared name. Falls back to the base type name.
const char* type_receiver_name(const Type* type) {
    if (!type) return NULL;
    if (type->kind == TYPE_POINTER && type->data.pointer.pointee_type) {
        type = type->data.pointer.pointee_type;
    }
    if (type->kind == TYPE_STRUCT && type->data.struct_type.name) {
        return type->data.struct_type.name;
    }
    return type->name;
}

// Variable management

Variable* variable_new(const char* name, Type* type, Position pos) {
    Variable* var = malloc(sizeof(Variable));
    if (!var) return NULL;

    var->name = str_dup(name);
    var->type = type;
    var->ownership = OWNERSHIP_OWNED;  // Default ownership
    var->mutability = MUTABILITY_IMMUTABLE;  // Default immutable
    var->is_moved = 0;
    var->is_borrowed = 0;
    var->borrow_count = 0;
    var->is_initialized = 0;
    var->is_builtin = 0;
    var->declared_pos = pos;
    var->comptime_value = NULL;  // populated by type_check_const_decl for is_comptime consts
    var->package = NULL;  // set only on TYPE_PACKAGE markers (stdlib Phase 0)
    var->next = NULL;
    var->decl_node = NULL;  // Closures Task 2: set by VarDeclNode-backed registration sites
    var->is_captured = 0;   // Closures Task 2: set by type_checker_record_capture
    var->is_loop_var = 0;   // Closures Task 2: set by type_check_for_stmt's loop-binding sites
    var->has_const_int_value = 0;  // fix/const-array-length: set by type_check_const_decl
    var->const_int_value = 0;
    var->is_generic = 0;       // Function generics Task 4: set by declare_function_signature
    var->generic_decl = NULL;
    var->type_param_count = 0;

    return var;
}

void variable_free(Variable* var) {
    if (var) {
        free(var->name);
        if (var->comptime_value) comptime_value_free(var->comptime_value);
        free(var);
    }
}

// Scope management

Scope* scope_new(Scope* parent) {
    Scope* scope = malloc(sizeof(Scope));
    if (!scope) return NULL;
    
    scope->variables = NULL;
    scope->parent = parent;
    scope->scope_id = parent ? parent->scope_id + 1 : 0;
    scope->is_function_boundary = 0;  // Closures Task 2: set explicitly by function/literal body pushes

    return scope;
}

void scope_free(Scope* scope) {
    if (!scope) return;
    
    Variable* var = scope->variables;
    while (var) {
        Variable* next = var->next;
        variable_free(var);
        var = next;
    }
    
    free(scope);
}

int scope_add_variable(Scope* scope, Variable* var) {
    if (!scope || !var) return 0;
    
    // Check for redeclaration in current scope
    Variable* existing = scope->variables;
    while (existing) {
        if (strcmp(existing->name, var->name) == 0) {
            return 0;  // Variable already exists
        }
        existing = existing->next;
    }
    
    // Add to front of list
    var->next = scope->variables;
    scope->variables = var;
    return 1;
}

Variable* scope_lookup_variable(Scope* scope, const char* name) {
    if (!scope || !name) return NULL;
    
    Variable* var = scope->variables;
    while (var) {
        if (strcmp(var->name, name) == 0) {
            return var;
        }
        var = var->next;
    }
    
    // Look in parent scope
    if (scope->parent) {
        return scope_lookup_variable(scope->parent, name);
    }
    
    return NULL;
}

// Error reporting

void type_error(TypeChecker* checker, Position pos, const char* format, ...) {
    if (!checker) return;
    
    va_list args;
    va_start(args, format);
    
    fprintf(stderr, "Type error at %s:%d:%d: ",
            pos.filename ? pos.filename : "<unknown>",
            pos.line, pos.column);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
    checker->error_count++;
}

void type_warning(TypeChecker* checker, Position pos, const char* format, ...) {
    if (!checker) return;
    
    va_list args;
    va_start(args, format);
    
    fprintf(stderr, "Type warning at %s:%d:%d: ",
            pos.filename ? pos.filename : "<unknown>",
            pos.line, pos.column);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
    checker->warning_count++;
}