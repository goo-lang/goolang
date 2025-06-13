#ifndef TYPES_H
#define TYPES_H

#include "ast.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct Type Type;
typedef struct TypeChecker TypeChecker;
typedef struct Scope Scope;

// Type kinds
typedef enum {
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_INT8,
    TYPE_INT16,
    TYPE_INT32,
    TYPE_INT64,
    TYPE_UINT8,
    TYPE_UINT16,
    TYPE_UINT32,
    TYPE_UINT64,
    TYPE_FLOAT32,
    TYPE_FLOAT64,
    TYPE_STRING,
    TYPE_CHAR,
    
    // Compound types
    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_MAP,
    TYPE_CHANNEL,
    TYPE_FUNCTION,
    TYPE_POINTER,
    TYPE_REFERENCE,
    TYPE_STRUCT,
    TYPE_INTERFACE,
    
    // Goo extensions
    TYPE_ERROR_UNION,
    TYPE_NULLABLE,
    TYPE_QUALIFIED,  // Type with ownership/mutability qualifiers
    
    TYPE_UNKNOWN,
    TYPE_COUNT
} TypeKind;

// Use the OwnershipKind from ast.h to avoid conflicts

// Mutability kinds
typedef enum {
    MUTABILITY_IMMUTABLE,
    MUTABILITY_MUTABLE
} MutabilityKind;

// Channel patterns (from AST)
// Already defined in ast.h as ChannelPattern

// Base type structure
struct Type {
    TypeKind kind;
    size_t size;        // Size in bytes
    size_t align;       // Alignment in bytes
    char* name;         // Type name (for debugging/error messages)
    
    // Type-specific data
    union {
        // Array type
        struct {
            Type* element_type;
            size_t length;
        } array;
        
        // Slice type
        struct {
            Type* element_type;
        } slice;
        
        // Map type
        struct {
            Type* key_type;
            Type* value_type;
        } map;
        
        // Channel type
        struct {
            Type* element_type;
            ChannelPattern pattern;
            char* endpoint;  // Optional network endpoint
        } channel;
        
        // Function type
        struct {
            Type** param_types;
            size_t param_count;
            Type* return_type;
            int is_variadic;
        } function;
        
        // Pointer type
        struct {
            Type* pointee_type;
        } pointer;
        
        // Reference type  
        struct {
            Type* referenced_type;
            int is_mutable;
        } reference;
        
        // Struct type
        struct {
            struct StructField* fields;
            size_t field_count;
            char* name;
        } struct_type;
        
        // Interface type
        struct {
            struct InterfaceMethod* methods;
            size_t method_count;
            char* name;
        } interface;
        
        // Error union type (!T)
        struct {
            Type* value_type;
            Type* error_type;  // Usually a standard error type
        } error_union;
        
        // Nullable type (?T)
        struct {
            Type* base_type;
        } nullable;
        
        // Qualified type (with ownership/mutability)
        struct {
            Type* base_type;
            OwnershipKind ownership;
            MutabilityKind mutability;
        } qualified;
    } data;
};

// Struct field
typedef struct StructField {
    char* name;
    Type* type;
    size_t offset;
    OwnershipKind ownership;
    MutabilityKind mutability;
} StructField;

// Interface method
typedef struct InterfaceMethod {
    char* name;
    Type* type;  // Function type
} InterfaceMethod;

// Variable information for type checking
typedef struct Variable {
    char* name;
    Type* type;
    OwnershipKind ownership;
    MutabilityKind mutability;
    int is_moved;           // For ownership tracking
    int is_borrowed;        // Currently borrowed
    int borrow_count;       // Number of active borrows
    int is_initialized;
    int is_builtin;         // Built-in function/variable
    Position declared_pos;
    struct Variable* next;  // For linked list in scope
} Variable;

// Scope for variable and type tracking
struct Scope {
    Variable* variables;
    struct Scope* parent;
    int scope_id;
};

// Type checker state
struct TypeChecker {
    Scope* current_scope;
    int next_scope_id;
    Type** builtin_types;   // Array of builtin types
    
    // Error reporting
    char* current_file;
    int error_count;
    int warning_count;
    
    // Type cache for performance
    Type** type_cache;
    size_t type_cache_size;
    size_t type_cache_capacity;
};

// Type creation functions
Type* type_new(TypeKind kind);
Type* type_void(void);
Type* type_bool(void);
Type* type_int(int bits, int is_signed);
Type* type_float(int bits);
Type* type_string_type(void);
Type* type_char(void);

Type* type_array(Type* element_type, size_t length);
Type* type_slice(Type* element_type);
Type* type_map(Type* key_type, Type* value_type);
Type* type_channel(Type* element_type, ChannelPattern pattern);
Type* type_function(Type** param_types, size_t param_count, Type* return_type);
Type* type_pointer(Type* pointee_type);
Type* type_reference(Type* referenced_type, int is_mutable);

// Goo extension types
Type* type_error_union(Type* value_type, Type* error_type);
Type* type_nullable(Type* base_type);
Type* type_qualified(Type* base_type, OwnershipKind ownership, MutabilityKind mutability);

// Type operations
void type_free(Type* type);
Type* type_copy(const Type* type);
int type_equals(const Type* a, const Type* b);
int type_compatible(const Type* from, const Type* to);
const char* type_to_string(const Type* type);
size_t type_size(const Type* type);
size_t type_align(const Type* type);

// Type checking utilities
int type_is_integer(const Type* type);
int type_is_float(const Type* type);
int type_is_numeric(const Type* type);
int type_is_signed(const Type* type);
int type_is_pointer_like(const Type* type);
int type_is_nullable(const Type* type);
int type_is_error_union(const Type* type);

// Type checker functions
TypeChecker* type_checker_new(void);
void type_checker_free(TypeChecker* checker);

// Scope management
Scope* scope_new(Scope* parent);
void scope_free(Scope* scope);
void scope_push(TypeChecker* checker);
void scope_pop(TypeChecker* checker);

// Variable management
Variable* variable_new(const char* name, Type* type, Position pos);
void variable_free(Variable* var);
int scope_add_variable(Scope* scope, Variable* var);
Variable* scope_lookup_variable(Scope* scope, const char* name);
Variable* type_checker_lookup_variable(TypeChecker* checker, const char* name);

// Type checking entry points
int type_check_program(TypeChecker* checker, ASTNode* program);
Type* type_check_expression(TypeChecker* checker, ASTNode* expr);
int type_check_statement(TypeChecker* checker, ASTNode* stmt);
int type_check_declaration(TypeChecker* checker, ASTNode* decl);

// Declaration type checking functions
int type_check_function_decl(TypeChecker* checker, ASTNode* decl);
int type_check_var_decl(TypeChecker* checker, ASTNode* decl);
int type_check_const_decl(TypeChecker* checker, ASTNode* decl);
int type_check_type_decl(TypeChecker* checker, ASTNode* decl);

// Statement type checking functions
int type_check_block_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_expr_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_if_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_for_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_return_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_go_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_select_stmt(TypeChecker* checker, ASTNode* stmt);

// Helper functions
Type* type_from_ast(TypeChecker* checker, ASTNode* type_node);

// Expression type checking functions
Type* type_check_identifier(TypeChecker* checker, ASTNode* expr);
Type* type_check_literal(TypeChecker* checker, ASTNode* expr);
Type* type_check_binary_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_unary_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_call_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_make_chan_call(TypeChecker* checker, CallExprNode* call, ASTNode* expr);
Type* type_check_index_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_selector_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_try_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_catch_expr(TypeChecker* checker, ASTNode* expr);

// Expression helper functions
Type* type_check_arithmetic_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos);
Type* type_check_comparison_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos);
Type* type_check_channel_send_op(TypeChecker* checker, Type* channel_type, Type* value_type, Position pos);
Type* type_check_channel_receive_op(TypeChecker* checker, Type* channel_type, Position pos);
Type* type_check_logical_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos);
Type* type_check_bitwise_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos);
Type* type_check_assignment_op(TypeChecker* checker, ASTNode* target, Type* target_type, Type* value_type, Position pos);

// Error reporting
void type_error(TypeChecker* checker, Position pos, const char* format, ...);
void type_warning(TypeChecker* checker, Position pos, const char* format, ...);

// Builtin types access
void type_checker_init_builtins(TypeChecker* checker);
void type_checker_add_builtin_functions(TypeChecker* checker);
Type* type_checker_get_builtin(TypeChecker* checker, TypeKind kind);

// Channel helper functions
const char* channel_pattern_string(ChannelPattern pattern);

#endif // TYPES_H