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
        case AST_MATCH_EXPR:
            return codegen_generate_match(codegen, checker, expr);
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
            // MapLitNode — `map[string]V{ … }`. Lowers to:
            //   m = goo_map_new_sv()
            //   for each (k,v): goo_map_set_sv(m, k_ptr, slot(v))
            // where slot(v) casts the declared V into the 8-byte i64 slot.
            // Returns the GooMapSV* as a raw ptr-typed value.
            MapLitNode* lit = (MapLitNode*)expr;
            Type* val_type = (expr->node_type && expr->node_type->kind == TYPE_MAP)
                ? expr->node_type->data.map.value_type : NULL;
            if (!val_type) {
                codegen_error(codegen, expr->pos, "map literal missing resolved value type");
                return NULL;
            }
            LLVMValueRef new_fn = LLVMGetNamedFunction(codegen->module, "goo_map_new_sv");
            LLVMValueRef set_fn = LLVMGetNamedFunction(codegen->module, "goo_map_set_sv");
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
                LLVMValueRef slot = codegen_map_value_to_slot(codegen, vv->llvm_value, val_type);
                LLVMValueRef args[3] = { m, kp, slot };
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

        case TOKEN_NIL:
            // nil literal: the var-decl initialiser path intercepts this before
            // reaching here when the declared type is ?T (see function_codegen.c).
            // For any other context fall back to a generic null i8* so the module
            // stays well-formed.
            return codegen_generate_null_literal(codegen, checker, NULL);

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

#if LLVM_AVAILABLE
// Compute the ADDRESS of an assignable lvalue without loading its value.
// codegen_generate_expression auto-loads identifiers (and a selector spills a
// loaded struct base to a throwaway temp), which is correct for reads but loses
// stores. This helper keeps everything as addresses so `x = v`, `s.field = v`,
// and `a[i] = v` write to the real storage. Returns a ValueInfo whose
// llvm_value is a pointer to the storage (is_lvalue == 1), or NULL if the
// expression is not an addressable lvalue.
static ValueInfo* codegen_emit_lvalue_address(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!expr) return NULL;

    if (expr->type == AST_IDENTIFIER) {
        IdentifierNode* ident = (IdentifierNode*)expr;
        ValueInfo* v = codegen_lookup_value(codegen, ident->name);
        if (!v || !v->is_lvalue) return NULL;
        return v; // the variable's alloca
    }

    if (expr->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* sel = (SelectorExprNode*)expr;
        ValueInfo* base = codegen_emit_lvalue_address(codegen, checker, sel->expr);
        if (!base) return NULL;

        // Resolve the struct's address. For a pointer-to-struct variable the
        // base alloca holds the pointer, so load it to reach the struct.
        Type* st = base->goo_type;
        LLVMValueRef struct_addr = base->llvm_value;
        if (st && st->kind == TYPE_POINTER && st->data.pointer.pointee_type &&
            st->data.pointer.pointee_type->kind == TYPE_STRUCT) {
            struct_addr = LLVMBuildLoad2(codegen->builder,
                                         LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                                         base->llvm_value, "struct_ptr");
            st = st->data.pointer.pointee_type;
        }
        if (!st || st->kind != TYPE_STRUCT) return NULL;

        int field_index = -1;
        StructField* field = NULL;
        for (size_t i = 0; i < st->data.struct_type.field_count; i++) {
            if (strcmp(st->data.struct_type.fields[i].name, sel->selector) == 0) {
                field_index = (int)i;
                field = &st->data.struct_type.fields[i];
                break;
            }
        }
        if (field_index == -1) return NULL;

        LLVMValueRef indices[] = {
            LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),
            LLVMConstInt(LLVMInt32TypeInContext(codegen->context), field_index, 0)
        };
        LLVMValueRef field_ptr = LLVMBuildGEP2(codegen->builder,
                                               codegen_type_to_llvm(codegen, st),
                                               struct_addr, indices, 2, sel->selector);
        ValueInfo* out = value_info_new(sel->selector, field_ptr, field->type);
        out->is_lvalue = 1;
        return out;
    }

    if (expr->type == AST_INDEX_EXPR) {
        IndexExprNode* ix = (IndexExprNode*)expr;
        ValueInfo* base = codegen_emit_lvalue_address(codegen, checker, ix->expr);
        if (!base) return NULL;
        Type* base_type = base->goo_type;
        if (!base_type) return NULL;

        ValueInfo* index_val = codegen_generate_expression(codegen, checker, ix->index);
        if (!index_val) return NULL;
        if (index_val->is_lvalue && index_val->goo_type) {
            LLVMTypeRef it = codegen_type_to_llvm(codegen, index_val->goo_type);
            if (it) {
                index_val->llvm_value = LLVMBuildLoad2(codegen->builder, it, index_val->llvm_value, "idx");
                index_val->is_lvalue = 0;
            }
        }

        if (base_type->kind == TYPE_ARRAY) {
            // base->llvm_value is a pointer to the array; GEP the element.
            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),
                index_val->llvm_value
            };
            LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder,
                                                  codegen_type_to_llvm(codegen, base_type),
                                                  base->llvm_value, indices, 2, "array_elem");
            ValueInfo* out = value_info_new(NULL, elem_ptr, base_type->data.array.element_type);
            out->is_lvalue = 1;
            return out;
        }

        if (base_type->kind == TYPE_SLICE) {
            // base->llvm_value points to the slice struct { ptr, len, cap }; load
            // it, take the data pointer (field 0), and GEP into the backing buffer.
            Type* elem_type = base_type->data.slice.element_type;
            LLVMValueRef slice_val = LLVMBuildLoad2(codegen->builder,
                                                    codegen_type_to_llvm(codegen, base_type),
                                                    base->llvm_value, "slice_load");
            LLVMValueRef data_ptr = LLVMBuildExtractValue(codegen->builder, slice_val, 0, "slice_ptr");
            LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder,
                                                  codegen_type_to_llvm(codegen, elem_type),
                                                  data_ptr, &index_val->llvm_value, 1, "slice_elem");
            ValueInfo* out = value_info_new(NULL, elem_ptr, elem_type);
            out->is_lvalue = 1;
            return out;
        }

        return NULL; // maps / pointers: not an addressable element lvalue here
    }

    if (expr->type == AST_UNARY_EXPR) {
        // Deref lvalue: `*p = v`. The store address is the pointer value
        // itself, so generate the operand (loading p to its pointer value) and
        // return that as the address. Element type is the pointee.
        UnaryExprNode* un = (UnaryExprNode*)expr;
        if (un->operator == TOKEN_MULTIPLY) {
            ValueInfo* ptr = codegen_generate_expression(codegen, checker, un->operand);
            if (!ptr || !ptr->goo_type || ptr->goo_type->kind != TYPE_POINTER) return NULL;
            ValueInfo* out = value_info_new(NULL, ptr->llvm_value,
                                            ptr->goo_type->data.pointer.pointee_type);
            out->is_lvalue = 1;
            return out;
        }
        return NULL;
    }

    return NULL;
}
#endif

// Widen (sign-extend) or truncate an integer value to a target LLVM type.
// Signed: Goo int literals are signed, so widening sign-extends.
static LLVMValueRef int_widen_or_trunc(CodeGenerator* codegen, LLVMValueRef v,
                                       LLVMTypeRef to_ty,
                                       unsigned from_bits, unsigned to_bits) {
    if (from_bits < to_bits)
        return LLVMBuildSExt(codegen->builder, v, to_ty, "litsext");
    if (from_bits > to_bits)
        return LLVMBuildTrunc(codegen->builder, v, to_ty, "littrunc");
    return v;
}

// Returns non-zero if `node` is a compile-time integer constant: either a
// bare AST_LITERAL or a unary minus applied to an AST_LITERAL (i.e. `-1`).
// The parser represents `-1` as AST_UNARY_EXPR(TOKEN_MINUS, AST_LITERAL),
// so both forms must be recognised as "untyped literal" for coercion.
static int is_int_literal_node(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_LITERAL) return 1;
    if (node->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)node;
        if (u->operator == TOKEN_MINUS && u->operand &&
            u->operand->type == AST_LITERAL)
            return 1;
    }
    return 0;
}

// If exactly one operand is an integer LITERAL and the operand widths differ,
// coerce the literal to the other operand's integer type. Go-faithful: only
// untyped literals adapt; two mismatched typed variables are left alone (that
// mismatch is a type error surfaced earlier, not silently coerced here).
static void coerce_int_literal_operand(CodeGenerator* codegen,
                                       ASTNode* left_ast, ValueInfo* left,
                                       ASTNode* right_ast, ValueInfo* right) {
    if (!left || !right || !left->goo_type || !right->goo_type) return;
    if (!type_is_integer(left->goo_type) || !type_is_integer(right->goo_type))
        return;
    LLVMTypeRef lt = LLVMTypeOf(left->llvm_value);
    LLVMTypeRef rt = LLVMTypeOf(right->llvm_value);
    if (LLVMGetTypeKind(lt) != LLVMIntegerTypeKind ||
        LLVMGetTypeKind(rt) != LLVMIntegerTypeKind) return;
    unsigned lw = LLVMGetIntTypeWidth(lt);
    unsigned rw = LLVMGetIntTypeWidth(rt);
    if (lw == rw) return;

    int left_lit  = is_int_literal_node(left_ast);
    int right_lit = is_int_literal_node(right_ast);
    if (left_lit == right_lit) return;  // neither, or both — leave alone

    if (left_lit) {
        left->llvm_value = int_widen_or_trunc(codegen, left->llvm_value, rt, lw, rw);
        left->goo_type   = right->goo_type;
    } else {
        right->llvm_value = int_widen_or_trunc(codegen, right->llvm_value, lt, rw, lw);
        right->goo_type   = left->goo_type;
    }
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
        // Map element assignment `m[k] = v` has no addressable lvalue — it
        // lowers to a goo_map_set_sv call. Intercept before lvalue resolution
        // (which rejects map index targets). Only the map case is handled
        // here; array/slice index assignment falls through to the GEP path.
        if (binary->left->type == AST_INDEX_EXPR) {
            IndexExprNode* idx = (IndexExprNode*)binary->left;
            Type* base_t = type_check_expression(checker, idx->expr);
            if (base_t && base_t->kind == TYPE_MAP) {
                Type* val_type = base_t->data.map.value_type;
                LLVMValueRef set_fn = LLVMGetNamedFunction(codegen->module, "goo_map_set_sv");
                if (!set_fn) {
                    codegen_error(codegen, expr->pos, "goo_map_set_sv missing");
                    return NULL;
                }
                ValueInfo* mv = codegen_generate_expression(codegen, checker, idx->expr);
                ValueInfo* kv = codegen_generate_expression(codegen, checker, idx->index);
                if (!mv || !kv) { value_info_free(mv); value_info_free(kv); return NULL; }
                LLVMValueRef kp = kv->llvm_value;
                if (kv->goo_type && kv->goo_type->kind == TYPE_STRING) {
                    kp = LLVMBuildExtractValue(codegen->builder, kp, 0, "k_ptr");
                }
                ValueInfo* vv = codegen_generate_expression(codegen, checker, binary->right);
                if (!vv) { value_info_free(mv); value_info_free(kv); return NULL; }
                if (vv->is_lvalue && vv->goo_type) {
                    LLVMTypeRef vt = codegen_type_to_llvm(codegen, vv->goo_type);
                    if (vt) {
                        vv->llvm_value = LLVMBuildLoad2(codegen->builder, vt, vv->llvm_value, "rval");
                        vv->is_lvalue = 0;
                    }
                }
                LLVMValueRef slot = codegen_map_value_to_slot(codegen, vv->llvm_value, val_type);
                LLVMValueRef args[3] = { mv->llvm_value, kp, slot };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(set_fn), set_fn, args, 3, "");
                value_info_free(mv);
                value_info_free(kv);
                return vv;  // assignment evaluates to the stored value
            }
        }

        // Resolve the assignment target's address. Three lvalue forms are
        // supported; each yields a ValueInfo whose llvm_value is a pointer to
        // the storage location (is_lvalue == 1):
        //   - identifier  -> the variable's alloca (codegen_lookup_value)
        //   - field s.x   -> a struct GEP (codegen_generate_selector_expr)
        //   - index a[i]  -> an element GEP (codegen_generate_index_expr)
        // The selector/index read paths already compute and return these GEPs
        // unloaded, so assignment reuses them directly.
        ValueInfo* target = codegen_emit_lvalue_address(codegen, checker, binary->left);
        if (!target || !target->is_lvalue) {
            codegen_error(codegen, expr->pos,
                          "Assignment target must be an addressable lvalue "
                          "(identifier, field, or index)");
            return NULL;
        }

        // Generate the right side value. If it is itself an lvalue (e.g.
        // `a[i] = b[j]` or `x = s.y`), dereference it to the scalar value
        // before storing — mirroring the auto-load consumers do elsewhere.
        ValueInfo* value = codegen_generate_expression(codegen, checker, binary->right);
        if (!value) return NULL;
        if (value->is_lvalue && value->goo_type) {
            LLVMTypeRef vt = codegen_type_to_llvm(codegen, value->goo_type);
            if (vt) {
                value->llvm_value = LLVMBuildLoad2(codegen->builder, vt, value->llvm_value, "rval");
                value->is_lvalue = 0;
            }
        }

        // Store the value into the target's address.
        LLVMBuildStore(codegen->builder, value->llvm_value, target->llvm_value);

        // Return the stored value
        return value;
    }
    
    // Special handling for channel send (<-)
    if (binary->operator == TOKEN_ARROW) {
        return codegen_generate_channel_send(codegen, checker, expr);
    }

    // Special handling for nullable == nil / nil == nullable.
    // We must NOT evaluate the nil side: codegen_generate_null_literal
    // without a context type emits an i8* null, which is the wrong LLVM
    // type to compare against a { i1, T } nullable struct. Instead, detect
    // the pattern here, evaluate ONLY the nullable operand, then read its
    // is_null flag directly.
    if (binary->operator == TOKEN_EQ || binary->operator == TOKEN_NE) {
        ASTNode* nullable_node = NULL;

        // Determine which side is the nil literal and which is nullable.
        bool right_is_nil = (binary->right->type == AST_LITERAL &&
                             ((LiteralNode*)binary->right)->literal_type == TOKEN_NIL);
        bool left_is_nil  = (binary->left->type == AST_LITERAL &&
                             ((LiteralNode*)binary->left)->literal_type == TOKEN_NIL);

        if (right_is_nil) {
            // Run the type checker on the left to confirm it is ?T.
            Type* lt = type_check_expression(checker, binary->left);
            if (lt && type_is_nullable(lt)) {
                nullable_node = binary->left;
            }
        } else if (left_is_nil) {
            // nil == ?T form
            Type* rt = type_check_expression(checker, binary->right);
            if (rt && type_is_nullable(rt)) {
                nullable_node = binary->right;
            }
        }

        if (nullable_node) {
            // Evaluate only the nullable operand.
            ValueInfo* nv = codegen_generate_expression(codegen, checker, nullable_node);
            if (!nv) return NULL;

            // Auto-load if the result is an lvalue (e.g. identifier/selector).
            if (nv->is_lvalue && nv->goo_type) {
                LLVMTypeRef nt = codegen_type_to_llvm(codegen, nv->goo_type);
                if (nt) {
                    nv->llvm_value = LLVMBuildLoad2(codegen->builder, nt, nv->llvm_value, "nullable_load");
                    nv->is_lvalue = 0;
                }
            }

            // Read the is_null flag (struct index 0) via the header-declared helper.
            LLVMValueRef is_null = codegen_check_nullable_null(codegen, nv->llvm_value);
            LLVMValueRef result;

            if (binary->operator == TOKEN_EQ) {
                // a == nil  →  is_null
                result = is_null;
            } else {
                // a != nil  →  !is_null
                result = LLVMBuildNot(codegen->builder, is_null, "is_not_null");
            }

            Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);
            value_info_free(nv);
            return value_info_new(NULL, result, bool_type);
        }
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

    // Auto-load the right operand too: a selector/index lvalue (e.g. the `p.y`
    // in `p.x + p.y`) returns the field address, so it must be dereferenced to
    // the scalar value before the binary op — mirroring the left operand above.
    if (right_val->is_lvalue && right_val->goo_type) {
        LLVMTypeRef rt = codegen_type_to_llvm(codegen, right_val->goo_type);
        if (rt) {
            right_val->llvm_value = LLVMBuildLoad2(codegen->builder, rt, right_val->llvm_value, "rval");
            right_val->is_lvalue = 0;
        }
    }

    // M3: reconcile an int literal operand's width to the other operand's
    // type so e.g. `int64_var == -1` compares i64-to-i64, not i64-to-i32.
    coerce_int_literal_operand(codegen, binary->left, left_val,
                               binary->right, right_val);

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
            // Dereference pointer. operand_llvm is the pointer value; the
            // pointee LLVM type comes from the goo type (LLVMGetElementType is
            // unusable under opaque pointers).
            if (operand->goo_type->kind == TYPE_POINTER) {
                LLVMTypeRef pointee = codegen_type_to_llvm(codegen, operand->goo_type->data.pointer.pointee_type);
                result = LLVMBuildLoad2(codegen->builder, pointee, operand_llvm, "deref");
            } else {
                codegen_error(codegen, expr->pos, "Cannot dereference non-pointer type");
                value_info_free(operand);
                return NULL;
            }
            break;

        case TOKEN_BIT_AND: {
            // Address-of (`&x`). The generic operand above was loaded (an
            // identifier auto-loads), so use the lvalue-address helper to get
            // the operand's storage address rather than its value.
            ValueInfo* addr = codegen_emit_lvalue_address(codegen, checker, unary->operand);
            if (!addr || !addr->is_lvalue) {
                codegen_error(codegen, expr->pos, "Cannot take address of non-lvalue");
                value_info_free(operand);
                return NULL;
            }
            result = addr->llvm_value;
            result_type = type_pointer(addr->goo_type);
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

