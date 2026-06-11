#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Expression code generation

ValueInfo* codegen_generate_expression(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!codegen || !checker || !expr) return NULL;
    
    switch (expr->type) {
        case AST_IDENTIFIER:
            return codegen_generate_identifier(codegen, checker, expr);
        case AST_LITERAL:
            return codegen_generate_literal(codegen, checker, expr);
        case AST_BINARY_EXPR:
            return codegen_generate_binary_expr(codegen, checker, expr);
        case AST_UNARY_EXPR:
            return codegen_generate_unary_expr(codegen, checker, expr);
        case AST_CALL_EXPR:
            return codegen_generate_call_expr(codegen, checker, expr);
        case AST_INDEX_EXPR:
            return codegen_generate_index_expr(codegen, checker, expr);
        case AST_SELECTOR_EXPR:
            return codegen_generate_selector_expr(codegen, checker, expr);
        case AST_TRY_EXPR:
            return codegen_generate_try_expr(codegen, checker, expr);
        case AST_CATCH_EXPR:
            return codegen_generate_catch_expr(codegen, checker, expr);
        case AST_PTR_ARITHMETIC:
            return codegen_generate_ptr_arithmetic(codegen, checker, expr);
        case AST_PTR_DEREF:
            return codegen_generate_ptr_deref(codegen, checker, expr);
        case AST_ADDR_OF:
            return codegen_generate_addr_of(codegen, checker, expr);
        case AST_PORT_IO:
            return codegen_generate_port_io(codegen, checker, expr);
        case AST_MMIO_ACCESS:
            return codegen_generate_mmio_access(codegen, checker, expr);
        case AST_SLICE_EXPR:
            return codegen_generate_slice_lit(codegen, checker, expr);
        case AST_STRUCT_LITERAL:
            return codegen_generate_struct_lit(codegen, checker, expr);
        case AST_POSTFIX_EXPR: {
            // `j++` / `j--`: load operand, compute load ± 1, store back,
            // return the LOADED (pre-modification) value. Postfix
            // semantics. Operand must be an lvalue (identifier or
            // selector). We don't auto-load the operand expression
            // because we need the alloca, not the value.
            PostfixExprNode* p = (PostfixExprNode*)expr;
            ValueInfo* operand;
            LLVMValueRef alloca_ref;
            LLVMTypeRef elem_llvm;
            LLVMValueRef loaded;
            LLVMValueRef one;
            LLVMValueRef updated;
            if (p->operand->type != AST_IDENTIFIER) {
                codegen_error(codegen, expr->pos, "postfix ++/-- requires a simple identifier (selector/index forms not yet supported)");
                return NULL;
            }
            operand = codegen_lookup_value(codegen, ((IdentifierNode*)p->operand)->name);
            if (!operand || !operand->is_lvalue) {
                codegen_error(codegen, expr->pos, "postfix ++/-- operand must be an lvalue");
                return NULL;
            }
            alloca_ref = operand->llvm_value;
            elem_llvm = operand->goo_type
                ? codegen_type_to_llvm(codegen, operand->goo_type)
                : LLVMInt32TypeInContext(codegen->context);
            loaded = LLVMBuildLoad2(codegen->builder, elem_llvm, alloca_ref, "postfix_load");
            one = LLVMConstInt(elem_llvm, 1, 0);
            if (p->operator == TOKEN_INCREMENT) {
                updated = LLVMBuildAdd(codegen->builder, loaded, one, "postfix_inc");
            } else {
                updated = LLVMBuildSub(codegen->builder, loaded, one, "postfix_dec");
            }
            LLVMBuildStore(codegen->builder, updated, alloca_ref);
            return value_info_new(NULL, loaded, operand->goo_type);
        }
        case AST_PAREN_EXPR: {
            // MapLitNode — `map[K]V{ … }`. Lowers to:
            //   m = goo_map_new_si()
            //   for each (k,v): goo_map_set_si(m, k_ptr, v)
            // Returns the GooMapSI* as a raw ptr-typed value.
            MapLitNode* lit = (MapLitNode*)expr;
            LLVMValueRef new_fn = LLVMGetNamedFunction(codegen->module, "goo_map_new_si");
            LLVMValueRef set_fn = LLVMGetNamedFunction(codegen->module, "goo_map_set_si");
            if (!new_fn || !set_fn) {
                codegen_error(codegen, expr->pos, "map runtime symbols missing");
                return NULL;
            }
            LLVMValueRef m = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(new_fn),
                                            new_fn, NULL, 0, "map_new");
            ASTNode* k = lit->keys;
            ASTNode* v = lit->values;
            while (k && v) {
                ValueInfo* kv = codegen_generate_expression(codegen, checker, k);
                ValueInfo* vv = codegen_generate_expression(codegen, checker, v);
                if (!kv || !vv) return NULL;
                LLVMValueRef kp = kv->llvm_value;
                if (kv->goo_type && kv->goo_type->kind == TYPE_STRING) {
                    kp = LLVMBuildExtractValue(codegen->builder, kp, 0, "k_ptr");
                }
                LLVMValueRef args[3] = { m, kp, vv->llvm_value };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(set_fn),
                              set_fn, args, 3, "");
                value_info_free(kv);
                value_info_free(vv);
                k = k->next;
                v = v->next;
            }
            return value_info_new(NULL, m, expr->node_type);
        }
        default:
            codegen_error(codegen, expr->pos, "Unknown expression type for code generation");
            return NULL;
    }
}

ValueInfo* codegen_generate_identifier(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_IDENTIFIER) return NULL;
    
    IdentifierNode* ident = (IdentifierNode*)expr;

    // Look up the identifier in the symbol table
    ValueInfo* value_info = codegen_lookup_value(codegen, ident->name);
    if (!value_info) {
        // Fall back to module-level functions. User-defined functions
        // (`func add(...)`) are registered via LLVMAddFunction during
        // codegen_generate_function_decl but never make it into the
        // codegen value table, so any `add(2,3)` call would fail here.
        LLVMValueRef func_val = LLVMGetNamedFunction(codegen->module, ident->name);
        if (func_val) {
            Variable* func_var = type_checker_lookup_variable(checker, ident->name);
            Type* func_type = func_var ? func_var->type : NULL;
            return value_info_new(ident->name, func_val, func_type);
        }
        codegen_error(codegen, expr->pos, "Undefined identifier '%s'", ident->name);
        return NULL;
    }
    
    // If it's an lvalue (variable), load the value. With LLVM 22 opaque
    // pointers, LLVMTypeOf(alloca) returns just `ptr`, not `ptr i32` —
    // we have to supply the load type from goo_type. Previously every
    // var read produced `load ptr, ptr %x` which fed `ptr` values into
    // `icmp sgt`/`add`, failing module verification.
    // A global const (codegen_const_decl creates one via LLVMAddGlobal) lives
    // in the value table as is_lvalue=0 but its LLVM value is the address, not
    // the contents. Reading it as an expression must load through that address
    // — otherwise `os.Exit(X)` passes `ptr @X` to a function expecting `i32`
    // and module verification rejects it (M9-const-ref-load).
    bool needs_load = value_info->is_lvalue
        || (LLVMIsAGlobalVariable(value_info->llvm_value) && value_info->goo_type);
    if (needs_load) {
        LLVMTypeRef load_type = value_info->goo_type
            ? codegen_type_to_llvm(codegen, value_info->goo_type)
            : LLVMTypeOf(value_info->llvm_value);
        LLVMValueRef loaded_value = LLVMBuildLoad2(codegen->builder, load_type, value_info->llvm_value, ident->name);
        return value_info_new(ident->name, loaded_value, value_info->goo_type);
    } else {
        // Direct value (function pointer, inlined literal, etc.)
        return value_info_new(ident->name, value_info->llvm_value, value_info->goo_type);
    }
#endif
}

ValueInfo* codegen_generate_literal(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_LITERAL) return NULL;
    
    LiteralNode* literal = (LiteralNode*)expr;
    LLVMValueRef llvm_value = NULL;
    Type* goo_type = NULL;
    
    switch (literal->literal_type) {
        case TOKEN_INT: {
            // Parse integer value from string
            long long value = atoll(literal->value);
            llvm_value = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), value, 1);
            goo_type = type_checker_get_builtin(checker, TYPE_INT32);
            break;
        }
        
        case TOKEN_FLOAT: {
            // Parse float value from string
            double value = atof(literal->value);
            llvm_value = LLVMConstReal(LLVMDoubleTypeInContext(codegen->context), value);
            goo_type = type_checker_get_builtin(checker, TYPE_FLOAT64);
            break;
        }
        
        case TOKEN_STRING: {
            // Create global string constant
            LLVMValueRef str_const = LLVMBuildGlobalStringPtr(codegen->builder, literal->value, "str");

            // Create string struct { ptr, len }
            LLVMTypeRef string_type = codegen_get_basic_type(codegen, TYPE_STRING);
            LLVMValueRef string_val = LLVMGetUndef(string_type);

            // Set pointer
            string_val = LLVMBuildInsertValue(codegen->builder, string_val, str_const, 0, "");

            // Set length
            size_t len = strlen(literal->value);
            LLVMValueRef len_val = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), len, 0);
            string_val = LLVMBuildInsertValue(codegen->builder, string_val, len_val, 1, "");

            llvm_value = string_val;
            goo_type = type_checker_get_builtin(checker, TYPE_STRING);
            break;
        }
        
        case TOKEN_TRUE:
        case TOKEN_FALSE: {
            int bool_val = (literal->literal_type == TOKEN_TRUE) ? 1 : 0;
            llvm_value = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), bool_val, 0);
            goo_type = type_checker_get_builtin(checker, TYPE_BOOL);
            break;
        }
        
        case TOKEN_CHAR: {
            // Parse character value (for now, just take first character)
            char char_val = literal->value[0];
            llvm_value = LLVMConstInt(LLVMInt8TypeInContext(codegen->context), char_val, 0);
            goo_type = type_checker_get_builtin(checker, TYPE_CHAR);
            break;
        }
        
        default:
            codegen_error(codegen, expr->pos, "Unknown literal type");
            return NULL;
    }
    
    if (!llvm_value || !goo_type) {
        codegen_error(codegen, expr->pos, "Failed to generate literal");
        return NULL;
    }
    
    return value_info_new(NULL, llvm_value, goo_type);
#endif
}

ValueInfo* codegen_generate_binary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_BINARY_EXPR) return NULL;
    
    BinaryExprNode* binary = (BinaryExprNode*)expr;
    
    // Special handling for assignment
    if (binary->operator == TOKEN_ASSIGN) {
        // Left side must be an lvalue
        if (binary->left->type != AST_IDENTIFIER) {
            codegen_error(codegen, expr->pos, "Assignment target must be an identifier");
            return NULL;
        }
        
        IdentifierNode* ident = (IdentifierNode*)binary->left;
        ValueInfo* target = codegen_lookup_value(codegen, ident->name);
        if (!target || !target->is_lvalue) {
            codegen_error(codegen, expr->pos, "Invalid assignment target '%s'", ident->name);
            return NULL;
        }
        
        // Generate right side value
        ValueInfo* value = codegen_generate_expression(codegen, checker, binary->right);
        if (!value) return NULL;
        
        // Store the value
        LLVMBuildStore(codegen->builder, value->llvm_value, target->llvm_value);
        
        // Return the stored value
        return value;
    }
    
    // Special handling for channel send (<-)
    if (binary->operator == TOKEN_ARROW) {
        return codegen_generate_channel_send(codegen, checker, expr);
    }
    
    // Generate left and right operands
    ValueInfo* left_val = codegen_generate_expression(codegen, checker, binary->left);
    if (!left_val) return NULL;

    // Auto-load any lvalue (selector / index / parameter / var read)
    // before using it as a value. codegen_generate_identifier loads
    // automatically; codegen_generate_selector_expr returns the field
    // address as an lvalue so it can also serve assignment targets,
    // which means binary_expr (and similar consumers) have to
    // dereference it themselves to get the actual scalar value.
    if (left_val->is_lvalue && left_val->goo_type) {
        LLVMTypeRef lt = codegen_type_to_llvm(codegen, left_val->goo_type);
        if (lt) {
            left_val->llvm_value = LLVMBuildLoad2(codegen->builder, lt, left_val->llvm_value, "lval");
            left_val->is_lvalue = 0;
        }
    }

    ValueInfo* right_val = codegen_generate_expression(codegen, checker, binary->right);
    if (!right_val) {
        value_info_free(left_val);
        return NULL;
    }
    
    // Get the result type from the type checker
    Type* result_type = type_check_binary_expr(checker, expr);
    if (!result_type) {
        value_info_free(left_val);
        value_info_free(right_val);
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    LLVMValueRef left_llvm = left_val->llvm_value;
    LLVMValueRef right_llvm = right_val->llvm_value;
    
    // Generate operation based on operator and types
    switch (binary->operator) {
        // Arithmetic operators
        case TOKEN_PLUS:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildAdd(codegen->builder, left_llvm, right_llvm, "add");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFAdd(codegen->builder, left_llvm, right_llvm, "fadd");
            }
            break;
            
        case TOKEN_MINUS:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildSub(codegen->builder, left_llvm, right_llvm, "sub");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFSub(codegen->builder, left_llvm, right_llvm, "fsub");
            }
            break;
            
        case TOKEN_MULTIPLY:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildMul(codegen->builder, left_llvm, right_llvm, "mul");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFMul(codegen->builder, left_llvm, right_llvm, "fmul");
            }
            break;
            
        case TOKEN_DIVIDE:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildSDiv(codegen->builder, left_llvm, right_llvm, "sdiv");
                } else {
                    result = LLVMBuildUDiv(codegen->builder, left_llvm, right_llvm, "udiv");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFDiv(codegen->builder, left_llvm, right_llvm, "fdiv");
            }
            break;
            
        case TOKEN_MODULO:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildSRem(codegen->builder, left_llvm, right_llvm, "srem");
                } else {
                    result = LLVMBuildURem(codegen->builder, left_llvm, right_llvm, "urem");
                }
            }
            break;
            
        // Comparison operators
        case TOKEN_EQ:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildICmp(codegen->builder, LLVMIntEQ, left_llvm, right_llvm, "eq");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOEQ, left_llvm, right_llvm, "feq");
            }
            break;
            
        case TOKEN_NE:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildICmp(codegen->builder, LLVMIntNE, left_llvm, right_llvm, "ne");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealONE, left_llvm, right_llvm, "fne");
            }
            break;
            
        case TOKEN_LT:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSLT, left_llvm, right_llvm, "slt");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntULT, left_llvm, right_llvm, "ult");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOLT, left_llvm, right_llvm, "flt");
            }
            break;
            
        case TOKEN_LE:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSLE, left_llvm, right_llvm, "sle");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntULE, left_llvm, right_llvm, "ule");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOLE, left_llvm, right_llvm, "fle");
            }
            break;
            
        case TOKEN_GT:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSGT, left_llvm, right_llvm, "sgt");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntUGT, left_llvm, right_llvm, "ugt");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOGT, left_llvm, right_llvm, "fgt");
            }
            break;
            
        case TOKEN_GE:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSGE, left_llvm, right_llvm, "sge");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntUGE, left_llvm, right_llvm, "uge");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOGE, left_llvm, right_llvm, "fge");
            }
            break;
            
        // Logical operators
        case TOKEN_AND:
            result = LLVMBuildAnd(codegen->builder, left_llvm, right_llvm, "and");
            break;
            
        case TOKEN_OR:
            result = LLVMBuildOr(codegen->builder, left_llvm, right_llvm, "or");
            break;
            
        // Bitwise operators
        case TOKEN_BIT_AND:
            result = LLVMBuildAnd(codegen->builder, left_llvm, right_llvm, "bitand");
            break;
            
        case TOKEN_BIT_OR:
            result = LLVMBuildOr(codegen->builder, left_llvm, right_llvm, "bitor");
            break;
            
        case TOKEN_BIT_XOR:
            result = LLVMBuildXor(codegen->builder, left_llvm, right_llvm, "xor");
            break;
            
        case TOKEN_LSHIFT:
            result = LLVMBuildShl(codegen->builder, left_llvm, right_llvm, "shl");
            break;
            
        case TOKEN_RSHIFT:
            if (type_is_signed(left_val->goo_type)) {
                result = LLVMBuildAShr(codegen->builder, left_llvm, right_llvm, "ashr");
            } else {
                result = LLVMBuildLShr(codegen->builder, left_llvm, right_llvm, "lshr");
            }
            break;
            
        default:
            codegen_error(codegen, expr->pos, "Unsupported binary operator");
            value_info_free(left_val);
            value_info_free(right_val);
            return NULL;
    }
    
    value_info_free(left_val);
    value_info_free(right_val);
    
    if (!result) {
        codegen_error(codegen, expr->pos, "Failed to generate binary operation");
        return NULL;
    }
    
    return value_info_new(NULL, result, result_type);
#endif
}

ValueInfo* codegen_generate_unary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_UNARY_EXPR) return NULL;
    
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    
    // Special handling for channel receive
    if (unary->operator == TOKEN_ARROW) {
        return codegen_generate_channel_recv(codegen, checker, expr);
    }
    
    // Generate operand
    ValueInfo* operand = codegen_generate_expression(codegen, checker, unary->operand);
    if (!operand) return NULL;
    
    // Get result type
    Type* result_type = type_check_unary_expr(checker, expr);
    if (!result_type) {
        value_info_free(operand);
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    LLVMValueRef operand_llvm = operand->llvm_value;
    
    switch (unary->operator) {
        case TOKEN_PLUS:
            // Unary plus is a no-op
            result = operand_llvm;
            break;
            
        case TOKEN_MINUS:
            if (type_is_integer(operand->goo_type)) {
                result = LLVMBuildNeg(codegen->builder, operand_llvm, "neg");
            } else if (type_is_float(operand->goo_type)) {
                result = LLVMBuildFNeg(codegen->builder, operand_llvm, "fneg");
            }
            break;
            
        case TOKEN_NOT:
            result = LLVMBuildNot(codegen->builder, operand_llvm, "not");
            break;
            
        case TOKEN_MULTIPLY:
            // Dereference pointer
            if (operand->goo_type->kind == TYPE_POINTER) {
                result = LLVMBuildLoad2(codegen->builder, LLVMGetElementType(LLVMTypeOf(operand_llvm)), operand_llvm, "deref");
            } else {
                codegen_error(codegen, expr->pos, "Cannot dereference non-pointer type");
                value_info_free(operand);
                return NULL;
            }
            break;
            
        case TOKEN_AND: {
            // Address-of operator - operand must be an lvalue
            if (!operand->is_lvalue) {
                codegen_error(codegen, expr->pos, "Cannot take address of non-lvalue");
                value_info_free(operand);
                return NULL;
            }
            
            // The operand is already a pointer to the value (lvalue)
            result = operand->llvm_value;
            
            // Create pointer type
            Type* ptr_type = type_new(TYPE_POINTER);
            ptr_type->data.pointer.pointee_type = operand->goo_type;
            ptr_type->size = 8;  // Assuming 64-bit pointers
            ptr_type->align = 8;
            result_type = ptr_type;
            break;
        }
            
        default:
            codegen_error(codegen, expr->pos, "Unsupported unary operator");
            value_info_free(operand);
            return NULL;
    }
    
    value_info_free(operand);
    
    if (!result) {
        codegen_error(codegen, expr->pos, "Failed to generate unary operation");
        return NULL;
    }
    
    return value_info_new(NULL, result, result_type);
#endif
}

// Goo extension expressions

// Forward declarations for error union functions
ValueInfo* codegen_generate_try_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_catch_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

ValueInfo* codegen_generate_try_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    return codegen_generate_try_expr_impl(codegen, checker, expr);
}

ValueInfo* codegen_generate_catch_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    return codegen_generate_catch_expr_impl(codegen, checker, expr);
}

