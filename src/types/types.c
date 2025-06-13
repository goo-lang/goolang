#include "types.h"
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
        type->data.function.param_types = param_types;
        type->data.function.param_count = param_count;
        type->data.function.return_type = return_type;
        type->data.function.is_variadic = 0;
        type->size = sizeof(void*);  // Function is a pointer
        type->align = sizeof(void*);
        type->name = str_dup("func");  // TODO: Create full signature
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

// Type operations

void type_free(Type* type) {
    if (!type) return;
    
    free(type->name);
    
    // Free type-specific data
    switch (type->kind) {
        case TYPE_FUNCTION:
            free(type->data.function.param_types);
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
        
        default:
            return 1;  // Basic types are equal if kinds match
    }
}

int type_compatible(const Type* from, const Type* to) {
    if (type_equals(from, to)) return 1;
    
    // Handle implicit conversions
    if (type_is_numeric(from) && type_is_numeric(to)) {
        // Allow numeric conversions (with potential warnings)
        return 1;
    }
    
    // Handle nullable types
    if (to->kind == TYPE_NULLABLE) {
        return type_compatible(from, to->data.nullable.base_type);
    }
    
    // Handle ownership qualifiers
    if (from->kind == TYPE_QUALIFIED && to->kind == TYPE_QUALIFIED) {
        return type_compatible(from->data.qualified.base_type, to->data.qualified.base_type);
    }
    
    return 0;
}

const char* type_to_string(const Type* type) {
    return type ? type->name : "null";
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
    var->next = NULL;
    
    return var;
}

void variable_free(Variable* var) {
    if (var) {
        free(var->name);
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