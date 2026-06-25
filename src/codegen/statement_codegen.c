#include "codegen.h"
#include "comptime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Statement code generation: the statement dispatcher and every
// statement kind (block, expr, if, for, return, go, defer, select,
// unsafe, asm). Split from function_codegen.c (refactor, no behavior
// change) — declarations (func/var/const) stay there.


int codegen_generate_statement(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
    if (!codegen || !checker || !stmt) return 0;
    
    switch (stmt->type) {
        case AST_BLOCK_STMT:
            return codegen_generate_block_stmt(codegen, checker, stmt);
        case AST_EXPR_STMT:
            return codegen_generate_expr_stmt(codegen, checker, stmt);
        case AST_VAR_DECL:
            return codegen_generate_var_decl(codegen, checker, stmt);
        case AST_IF_STMT:
            return codegen_generate_if_stmt(codegen, checker, stmt);
        case AST_IF_LET_STMT: {
            // Desugar `if let v = expr { … } [else { … }]` to:
            //   evaluate expr (TYPE_NULLABLE struct {i1 is_null, T value})
            //   br is_null, .else_or_skip, .then
            //   .then: alloca v; v = ExtractValue 1; codegen then_stmt
            //   .else: codegen else_stmt if present
            //   .exit
            // Declarations are hoisted to the top of the block so the
            // CompCert build (C99-strict, no mid-block decls) accepts.
            IfLetStmtNode* il;
            ValueInfo* nv;
            LLVMValueRef raw;
            LLVMValueRef is_null;
            LLVMBasicBlockRef then_bb;
            LLVMBasicBlockRef else_bb;
            LLVMBasicBlockRef exit_bb;
            Type* inner_type;
            int then_ok;
            int else_ok;

            il = (IfLetStmtNode*)stmt;
            nv = codegen_generate_expression(codegen, checker, il->nullable_expr);
            if (!nv) return 0;
            raw = nv->llvm_value;
            if (nv->is_lvalue && nv->goo_type) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, nv->goo_type);
                if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "il_load");
            }
            is_null = LLVMBuildExtractValue(codegen->builder, raw, 0, "is_null");
            then_bb = codegen_create_block(codegen, "iflet.then");
            else_bb = codegen_create_block(codegen, "iflet.else");
            exit_bb = codegen_create_block(codegen, "iflet.exit");
            LLVMBuildCondBr(codegen->builder, is_null, else_bb, then_bb);

            codegen_set_insert_point(codegen, then_bb);
            inner_type = nv->goo_type ? nv->goo_type->data.nullable.base_type : NULL;
            scope_push(checker);
            if (il->var_name && inner_type) {
                LLVMTypeRef inner_llvm = codegen_type_to_llvm(codegen, inner_type);
                LLVMValueRef val = LLVMBuildExtractValue(codegen->builder, raw, 1, il->var_name);
                LLVMValueRef alloca_v = codegen_create_entry_alloca(codegen, inner_llvm, il->var_name);
                LLVMBuildStore(codegen->builder, val, alloca_v);
                ValueInfo* vi = value_info_new(il->var_name, alloca_v, inner_type);
                vi->is_lvalue = 1; vi->is_initialized = 1;
                codegen_add_value(codegen, vi);
                {
                    Variable* tv = variable_new(il->var_name, inner_type, stmt->pos);
                    if (tv) { tv->is_initialized = 1; scope_add_variable(checker->current_scope, tv); }
                }
            }
            then_ok = il->then_stmt ? codegen_generate_statement(codegen, checker, il->then_stmt) : 1;
            scope_pop(checker);
            LLVMBuildBr(codegen->builder, exit_bb);

            codegen_set_insert_point(codegen, else_bb);
            else_ok = il->else_stmt ? codegen_generate_statement(codegen, checker, il->else_stmt) : 1;
            LLVMBuildBr(codegen->builder, exit_bb);

            codegen_set_insert_point(codegen, exit_bb);
            value_info_free(nv);
            return then_ok && else_ok;
        }
        case AST_FOR_STMT:
            return codegen_generate_for_stmt(codegen, checker, stmt);
        case AST_RETURN_STMT:
            return codegen_generate_return_stmt(codegen, checker, stmt);
        case AST_GO_STMT:
            return codegen_generate_go_stmt(codegen, checker, stmt);
        case AST_DEFER_STMT:
            return codegen_generate_defer_stmt(codegen, checker, stmt);
        case AST_SELECT_STMT:
            return codegen_generate_select_stmt(codegen, checker, stmt);
        case AST_SWITCH_STMT:
            return codegen_generate_switch_stmt(codegen, checker, stmt);
        case AST_UNSAFE_STMT:
            return codegen_generate_unsafe_stmt(codegen, checker, stmt);
        case AST_ASM_STMT:
            return codegen_generate_asm_stmt(codegen, checker, stmt);
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            // TODO: Implement break/continue
            return 1;
        case AST_COMPTIME_BLOCK: {
            // M11-block-dispatch: `comptime { ... }` blocks produce no
            // runtime code in the MVP scope. The type checker already
            // validated the body in subtask M11-types-const-stub; here
            // we simply emit nothing for the block, treating it as a
            // pure compile-time-only construct. A future task can lift
            // the values escaping from a comptime block to outer-scope
            // constants — out of scope for the MVP.
            return 1;
        }
        default:
            codegen_error(codegen, stmt->pos, "Unknown statement type for code generation");
            return 0;
    }
}

int codegen_generate_block_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_BLOCK_STMT) return 0;
    
    BlockStmtNode* block = (BlockStmtNode*)stmt;
    
    // Generate code for each statement in the block
    ASTNode* current = block->statements;
    while (current) {
        if (!codegen_generate_statement(codegen, checker, current)) {
            return 0;
        }
        current = current->next;
    }
    
    return 1;
#endif
}

int codegen_generate_expr_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_EXPR_STMT) return 0;
    
    ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
    
    // Generate the expression (result is discarded)
    ValueInfo* result = codegen_generate_expression(codegen, checker, expr_stmt->expr);
    if (!result) {
        return 0;
    }
    
    value_info_free(result);
    return 1;
#endif
}

int codegen_generate_if_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_IF_STMT) return 0;
    
    IfStmtNode* if_stmt = (IfStmtNode*)stmt;
    
    // Generate condition
    ValueInfo* condition = codegen_generate_expression(codegen, checker, if_stmt->condition);
    if (!condition) {
        return 0;
    }
    
    // Create basic blocks
    LLVMBasicBlockRef then_block = codegen_create_block(codegen, "if.then");
    LLVMBasicBlockRef else_block = if_stmt->else_stmt ? codegen_create_block(codegen, "if.else") : NULL;
    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "if.merge");
    
    // Generate conditional branch
    LLVMValueRef cond_val = condition->llvm_value;
    value_info_free(condition);
    
    LLVMBuildCondBr(codegen->builder, cond_val, then_block, else_block ? else_block : merge_block);
    
    // Generate then block
    codegen_set_insert_point(codegen, then_block);
    if (!codegen_generate_statement(codegen, checker, if_stmt->then_stmt)) {
        return 0;
    }
    
    // Branch to merge block if no terminator
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        LLVMBuildBr(codegen->builder, merge_block);
    }
    
    // Generate else block if present
    if (else_block) {
        codegen_set_insert_point(codegen, else_block);
        if (!codegen_generate_statement(codegen, checker, if_stmt->else_stmt)) {
            return 0;
        }
        
        // Branch to merge block if no terminator
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            LLVMBuildBr(codegen->builder, merge_block);
        }
    }
    
    // Continue with merge block
    codegen_set_insert_point(codegen, merge_block);
    
    return 1;
#endif
}

#if LLVM_AVAILABLE
// Load an rvalue from a ValueInfo, dereferencing if it is an lvalue. Mirrors
// the load logic used by the if-let lowering so switch tags and case
// expressions that resolve to variables compare by value, not by address.
static LLVMValueRef switch_rvalue(CodeGenerator* codegen, ValueInfo* vi) {
    LLVMValueRef v = vi->llvm_value;
    if (vi->is_lvalue && vi->goo_type) {
        LLVMTypeRef lt = codegen_type_to_llvm(codegen, vi->goo_type);
        if (lt) v = LLVMBuildLoad2(codegen->builder, lt, v, "switch.load");
    }
    return v;
}
#endif

// Expression switch lowering. A Go switch has no implicit fallthrough, so it
// lowers to a chain of equality comparisons: each case expression is compared
// against the tag, branching to that clause's body on a match or to the next
// test otherwise. If no case matches, control flows to the default clause (if
// present) or to the merge block. Each clause body branches to merge.
int codegen_generate_switch_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for switch statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_SWITCH_STMT) return 0;

    SwitchStmtNode* sw = (SwitchStmtNode*)stmt;

    // Evaluate the tag once, up front.
    ValueInfo* tag_vi = codegen_generate_expression(codegen, checker, sw->tag);
    if (!tag_vi) return 0;
    LLVMValueRef tag_val = switch_rvalue(codegen, tag_vi);
    value_info_free(tag_vi);

    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "switch.merge");

    size_t clause_count = 0;
    for (ASTNode* c = sw->cases; c; c = c->next) clause_count++;
    if (clause_count == 0) {
        LLVMBuildBr(codegen->builder, merge_block);
        codegen_set_insert_point(codegen, merge_block);
        return 1;
    }

    // One body block per clause; remember the default clause's body if any.
    LLVMBasicBlockRef* body_blocks = malloc(sizeof(LLVMBasicBlockRef) * clause_count);
    if (!body_blocks) return 0;
    LLVMBasicBlockRef default_body = NULL;
    size_t i = 0;
    for (ASTNode* c = sw->cases; c; c = c->next, i++) {
        body_blocks[i] = codegen_create_block(codegen, "switch.case");
        if (((CaseClauseNode*)c)->exprs == NULL) default_body = body_blocks[i];
    }

    // Comparison chain: test each non-default clause's expressions in order.
    i = 0;
    for (ASTNode* c = sw->cases; c; c = c->next, i++) {
        CaseClauseNode* clause = (CaseClauseNode*)c;
        if (clause->exprs == NULL) continue;  // default tested last
        for (ASTNode* e = clause->exprs; e; e = e->next) {
            ValueInfo* ev = codegen_generate_expression(codegen, checker, e);
            if (!ev) { free(body_blocks); return 0; }
            LLVMValueRef cmp = LLVMBuildICmp(codegen->builder, LLVMIntEQ,
                                             tag_val, switch_rvalue(codegen, ev), "switch.cmp");
            value_info_free(ev);
            LLVMBasicBlockRef next_test = codegen_create_block(codegen, "switch.test");
            LLVMBuildCondBr(codegen->builder, cmp, body_blocks[i], next_test);
            codegen_set_insert_point(codegen, next_test);
        }
    }
    // Fell through every test: go to default body, else merge.
    LLVMBuildBr(codegen->builder, default_body ? default_body : merge_block);

    // Emit clause bodies. No implicit fallthrough: each body ends at merge.
    i = 0;
    for (ASTNode* c = sw->cases; c; c = c->next, i++) {
        CaseClauseNode* clause = (CaseClauseNode*)c;
        codegen_set_insert_point(codegen, body_blocks[i]);
        for (ASTNode* s = clause->body; s; s = s->next) {
            if (!codegen_generate_statement(codegen, checker, s)) { free(body_blocks); return 0; }
        }
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            LLVMBuildBr(codegen->builder, merge_block);
        }
    }

    codegen_set_insert_point(codegen, merge_block);
    free(body_blocks);
    return 1;
#endif
}

int codegen_generate_for_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_FOR_STMT) return 0;

    ForStmtNode* for_stmt = (ForStmtNode*)stmt;

    // For-range: desugar `for k[, v] := range expr { ... }` to an
    // indexed loop. Evaluate the range expression once, get its
    // length, then loop k from 0..len. If value_name is set, load
    // the element via the slice's underlying pointer + GEP.
    if (for_stmt->range_expr) {
        // Evaluate range expression once.
        ValueInfo* range_val = codegen_generate_expression(codegen, checker, for_stmt->range_expr);
        if (!range_val) return 0;
        // Auto-load if lvalue.
        LLVMValueRef raw = range_val->llvm_value;
        if (range_val->is_lvalue && range_val->goo_type) {
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, range_val->goo_type);
            if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "range_load");
        }
        // Extract data pointer (field 0) and length (field 1) — both
        // slices and (eventually) strings share this layout.
        LLVMValueRef data_ptr = LLVMBuildExtractValue(codegen->builder, raw, 0, "range_data");
        LLVMValueRef len64 = LLVMBuildExtractValue(codegen->builder, raw, 1, "range_len");

        Type* elem_type = range_val->goo_type && range_val->goo_type->kind == TYPE_SLICE
            ? range_val->goo_type->data.slice.element_type
            : NULL;
        LLVMTypeRef llvm_elem = elem_type ? codegen_type_to_llvm(codegen, elem_type) : NULL;

        // Allocate index var; register it in scope under key_name.
        LLVMTypeRef i32 = LLVMInt32TypeInContext(codegen->context);
        LLVMValueRef idx_alloca = codegen_create_entry_alloca(codegen, i32,
                                                              for_stmt->key_name ? for_stmt->key_name : "range_i");
        LLVMBuildStore(codegen->builder, LLVMConstInt(i32, 0, 0), idx_alloca);
        if (for_stmt->key_name) {
            ValueInfo* kv = value_info_new(for_stmt->key_name, idx_alloca,
                                          type_checker_get_builtin(checker, TYPE_INT32));
            kv->is_lvalue = 1;
            kv->is_initialized = 1;
            codegen_add_value(codegen, kv);
        }

        // Allocate value var (per-iteration). Mirrored to type-check
        // scope below.
        LLVMValueRef val_alloca = NULL;
        if (for_stmt->value_name && llvm_elem && elem_type) {
            val_alloca = codegen_create_entry_alloca(codegen, llvm_elem, for_stmt->value_name);
            ValueInfo* vv = value_info_new(for_stmt->value_name, val_alloca, elem_type);
            vv->is_lvalue = 1;
            vv->is_initialized = 1;
            codegen_add_value(codegen, vv);
        }

        // Mirror loop vars to type-checker scope.
        scope_push(checker);
        if (for_stmt->key_name) {
            Variable* kv = variable_new(for_stmt->key_name,
                                       type_checker_get_builtin(checker, TYPE_INT32), stmt->pos);
            if (kv) { kv->is_initialized = 1; scope_add_variable(checker->current_scope, kv); }
        }
        if (for_stmt->value_name && elem_type) {
            Variable* vv = variable_new(for_stmt->value_name, elem_type, stmt->pos);
            if (vv) { vv->is_initialized = 1; scope_add_variable(checker->current_scope, vv); }
        }

        LLVMBasicBlockRef rcond = codegen_create_block(codegen, "range.cond");
        LLVMBasicBlockRef rbody = codegen_create_block(codegen, "range.body");
        LLVMBasicBlockRef rexit = codegen_create_block(codegen, "range.exit");

        LLVMBuildBr(codegen->builder, rcond);
        // cond: i < len  (compare in i64 to match len)
        codegen_set_insert_point(codegen, rcond);
        LLVMValueRef i_loaded = LLVMBuildLoad2(codegen->builder, i32, idx_alloca, "i");
        LLVMValueRef i64_widened = LLVMBuildSExt(codegen->builder, i_loaded,
                                                 LLVMInt64TypeInContext(codegen->context), "i64");
        LLVMValueRef cond_v = LLVMBuildICmp(codegen->builder, LLVMIntSLT, i64_widened, len64, "range_cond");
        LLVMBuildCondBr(codegen->builder, cond_v, rbody, rexit);

        // body: optionally load element, then run body
        codegen_set_insert_point(codegen, rbody);
        if (val_alloca && llvm_elem) {
            // elem_ptr = GEP data_ptr, i
            LLVMValueRef indices[] = { i_loaded };
            LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder, llvm_elem,
                                                  data_ptr, indices, 1, "elem_ptr");
            LLVMValueRef elem_val = LLVMBuildLoad2(codegen->builder, llvm_elem, elem_ptr, "elem");
            LLVMBuildStore(codegen->builder, elem_val, val_alloca);
        }
        int body_ok = 1;
        if (for_stmt->body) {
            body_ok = codegen_generate_statement(codegen, checker, for_stmt->body);
        }
        // i = i + 1
        LLVMValueRef i_now = LLVMBuildLoad2(codegen->builder, i32, idx_alloca, "i_inc");
        LLVMValueRef i_next = LLVMBuildAdd(codegen->builder, i_now,
                                           LLVMConstInt(i32, 1, 0), "i_next");
        LLVMBuildStore(codegen->builder, i_next, idx_alloca);
        LLVMBuildBr(codegen->builder, rcond);

        codegen_set_insert_point(codegen, rexit);
        scope_pop(checker);
        value_info_free(range_val);
        return body_ok;
    }

    // Create basic blocks
    LLVMBasicBlockRef init_block = codegen_create_block(codegen, "for.init");
    LLVMBasicBlockRef cond_block = codegen_create_block(codegen, "for.cond");
    LLVMBasicBlockRef body_block = codegen_create_block(codegen, "for.body");
    LLVMBasicBlockRef post_block = codegen_create_block(codegen, "for.post");
    LLVMBasicBlockRef exit_block = codegen_create_block(codegen, "for.exit");
    
    // Jump to init block
    LLVMBuildBr(codegen->builder, init_block);
    
    // Generate init block
    codegen_set_insert_point(codegen, init_block);
    if (for_stmt->init) {
        if (!codegen_generate_statement(codegen, checker, for_stmt->init)) {
            return 0;
        }
    }
    LLVMBuildBr(codegen->builder, cond_block);
    
    // Generate condition block
    codegen_set_insert_point(codegen, cond_block);
    if (for_stmt->condition) {
        ValueInfo* condition = codegen_generate_expression(codegen, checker, for_stmt->condition);
        if (!condition) {
            return 0;
        }
        
        LLVMBuildCondBr(codegen->builder, condition->llvm_value, body_block, exit_block);
        value_info_free(condition);
    } else {
        // Infinite loop
        LLVMBuildBr(codegen->builder, body_block);
    }
    
    // Generate body block
    codegen_set_insert_point(codegen, body_block);
    if (for_stmt->body) {
        if (!codegen_generate_statement(codegen, checker, for_stmt->body)) {
            return 0;
        }
    }
    LLVMBuildBr(codegen->builder, post_block);
    
    // Generate post block
    codegen_set_insert_point(codegen, post_block);
    if (for_stmt->post) {
        if (!codegen_generate_statement(codegen, checker, for_stmt->post)) {
            return 0;
        }
    }
    LLVMBuildBr(codegen->builder, cond_block);
    
    // Continue with exit block
    codegen_set_insert_point(codegen, exit_block);
    
    return 1;
#endif
}

#if LLVM_AVAILABLE
// Forward declaration for error return generation
LLVMValueRef codegen_generate_error_return(CodeGenerator* codegen, LLVMValueRef return_value, 
                                         Type* return_type, Type* function_return_type);
#endif

int codegen_generate_return_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_RETURN_STMT) return 0;
    
    ReturnStmtNode* return_stmt = (ReturnStmtNode*)stmt;
    
    if (return_stmt->values) {
        // Multi-value return: `return a, b` parses as values=[a]->next=b.
        // Detect 2+ values and build an anonymous struct via
        // LLVMBuildInsertValue, then ret that struct. The function's
        // declared return type is already a TYPE_STRUCT (anonymous,
        // from the parser's multi_return_type_list rule).
        if (return_stmt->values->next) {
            Type* function_return_type =
                codegen->current_function_info ? codegen->current_function_info->goo_type : NULL;
            if (!function_return_type || function_return_type->kind != TYPE_STRUCT) {
                codegen_error(codegen, stmt->pos,
                              "Multi-value return but function return type is not a tuple");
                return 0;
            }
            LLVMTypeRef ret_llvm = codegen_type_to_llvm(codegen, function_return_type);
            LLVMValueRef agg = LLVMGetUndef(ret_llvm);
            size_t i = 0;
            for (ASTNode* v = return_stmt->values; v; v = v->next, i++) {
                ValueInfo* vv = codegen_generate_expression(codegen, checker, v);
                if (!vv) return 0;
                LLVMValueRef raw = vv->llvm_value;
                if (vv->is_lvalue && vv->goo_type) {
                    LLVMTypeRef lt = codegen_type_to_llvm(codegen, vv->goo_type);
                    if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "ret_load");
                }
                agg = LLVMBuildInsertValue(codegen->builder, agg, raw, (unsigned)i, "ret_field");
                value_info_free(vv);
            }
            LLVMBuildRet(codegen->builder, agg);
            return 1;
        }

        // Single value return — original path.
        ValueInfo* return_value = codegen_generate_expression(codegen, checker, return_stmt->values);
        if (!return_value) {
            return 0;
        }

        // Auto-load an lvalue result (e.g. `return p.x`): a selector/index
        // returns the field address, which must be dereferenced to the scalar
        // value before being returned, or the function emits `ret ptr`.
#if LLVM_AVAILABLE
        if (return_value->is_lvalue && return_value->goo_type) {
            LLVMTypeRef rvt = codegen_type_to_llvm(codegen, return_value->goo_type);
            if (rvt) {
                return_value->llvm_value = LLVMBuildLoad2(codegen->builder, rvt, return_value->llvm_value, "retval");
                return_value->is_lvalue = 0;
            }
        }
#endif

        // Get function return type for error union handling
        Type* function_return_type = NULL;
        if (codegen->current_function_info && codegen->current_function_info->goo_type) {
            function_return_type = codegen->current_function_info->goo_type;
        }

        // Handle error union returns
#if LLVM_AVAILABLE
        LLVMValueRef final_return_value = return_value->llvm_value;
        if (function_return_type) {
            final_return_value = codegen_generate_error_return(codegen, return_value->llvm_value,
                                                             return_value->goo_type, function_return_type);
        }
        LLVMBuildRet(codegen->builder, final_return_value);
#else
        codegen_generate_error_return(codegen, return_value->llvm_value,
                                    return_value->goo_type, function_return_type);
#endif
        value_info_free(return_value);
    } else {
        // Bare return. If the enclosing function has a non-void LLVM signature
        // (e.g. the entry-point main, lowered to `i32 @main`), return a zero of
        // that type so the IR stays well-typed; otherwise a plain void return.
        LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(codegen->builder));
        LLVMTypeRef fn_ret = LLVMGetReturnType(LLVMGlobalGetValueType(cur_fn));
        if (LLVMGetTypeKind(fn_ret) != LLVMVoidTypeKind) {
            LLVMBuildRet(codegen->builder, LLVMConstNull(fn_ret));
        } else {
            LLVMBuildRetVoid(codegen->builder);
        }
    }
    
    return 1;
#endif
}

int codegen_generate_go_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for go statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_GO_STMT) return 0;
    
    GoStmtNode* go_stmt = (GoStmtNode*)stmt;
    
    // Handle WebAssembly-specific concurrency
    if (codegen_is_wasm_target(codegen)) {
        // In single-threaded WASM, transform goroutines to async/await
        // Generate JavaScript Promise-based execution
        
        if (go_stmt->call->type != AST_CALL_EXPR) {
            codegen_error(codegen, stmt->pos, "Go statement must contain a function call in WASM target");
            return 0;
        }
        
        CallExprNode* call = (CallExprNode*)go_stmt->call;
        
        // Create promise wrapper function
        LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
        LLVMValueRef create_promise_func = codegen_get_runtime_function(codegen, "js_create_promise");
        
        if (create_promise_func) {
            // Generate the function call as a promise
            ValueInfo* func_val = codegen_generate_expression(codegen, checker, call->function);
            if (!func_val) return 0;
            
            // Create promise with the function as executor
            LLVMValueRef args[] = { func_val->llvm_value };
            LLVMBuildCall2(codegen->builder, LLVMGetElementType(LLVMTypeOf(create_promise_func)),
                          create_promise_func, args, 1, "async_call");
            
            value_info_free(func_val);
            return 1;
        }
    }
    
    // Standard goroutine implementation for native targets
    // For go statements, we need to call goo_go with the function and arguments
    // This is a simplified implementation that assumes the call is a simple function call
    
    if (go_stmt->call->type != AST_CALL_EXPR) {
        codegen_error(codegen, stmt->pos, "Go statement must contain a function call");
        return 0;
    }
    
    CallExprNode* call = (CallExprNode*)go_stmt->call;
    
    // Generate the function address
    ValueInfo* func_val = codegen_generate_expression(codegen, checker, call->function);
    if (!func_val) return 0;
    
    // For simplicity, we'll use NULL as the argument for now
    // In a complete implementation, we'd need to package the arguments properly
    LLVMValueRef null_arg = LLVMConstNull(LLVMPointerType(LLVMInt8Type(), 0));
    
    // Get the goo_go function
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMTypeRef func_ptr_type = LLVMPointerType(LLVMFunctionType(LLVMVoidType(), &void_ptr_type, 1, 0), 0);
    LLVMTypeRef param_types[] = { func_ptr_type, void_ptr_type };
    LLVMTypeRef goo_go_type = LLVMFunctionType(void_ptr_type, param_types, 2, 0);
    
    LLVMValueRef goo_go_func = LLVMGetNamedFunction(codegen->module, "goo_go");
    if (!goo_go_func) {
        // Declare goo_go if not already declared
        goo_go_func = LLVMAddFunction(codegen->module, "goo_go", goo_go_type);
    }
    
    // Call goo_go(func, arg)
    LLVMValueRef args[] = { func_val->llvm_value, null_arg };
    LLVMBuildCall2(codegen->builder, goo_go_type, goo_go_func, args, 2, "");
    
    value_info_free(func_val);
    return 1;
#endif
}

int codegen_generate_defer_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for defer statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_DEFER_STMT) return 0;
    
    // TODO: Implement defer statements
    // For now, just treat it as a regular function call for compilation purposes
    DeferStmtNode* defer_stmt = (DeferStmtNode*)stmt;
    
    // Generate the deferred call as a regular expression for now
    ValueInfo* result = codegen_generate_expression(codegen, checker, defer_stmt->call);
    if (result) {
        value_info_free(result);
    }
    
    return 1;
#endif
}

int codegen_generate_select_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for select statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_SELECT_STMT) return 0;
    
    SelectStmtNode* select_stmt = (SelectStmtNode*)stmt;
    
    // Count the number of cases
    size_t case_count = 0;
    ASTNode* case_node = select_stmt->cases;
    while (case_node) {
        case_count++;
        case_node = case_node->next;
    }
    
    if (case_count == 0) {
        codegen_error(codegen, stmt->pos, "Select statement must have at least one case");
        return 0;
    }
    
    // Create array of select cases
    LLVMTypeRef select_case_type = codegen_get_select_case_type(codegen);
    LLVMValueRef cases_array = LLVMBuildArrayAlloca(codegen->builder, select_case_type, 
                                                   LLVMConstInt(LLVMInt64Type(), case_count, 0), 
                                                   "select_cases");
    
    // Create basic blocks for each case and the end
    LLVMBasicBlockRef* case_blocks = malloc(sizeof(LLVMBasicBlockRef) * case_count);
    LLVMBasicBlockRef default_block = NULL;
    LLVMBasicBlockRef end_block = LLVMAppendBasicBlock(codegen->current_function, "select_end");
    
    // Generate case blocks
    case_node = select_stmt->cases;
    size_t case_index = 0;
    int has_default = 0;
    
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;
        
        if (select_case->comm == NULL) {
            // Default case
            if (has_default) {
                codegen_error(codegen, case_node->pos, "Select statement can only have one default case");
                free(case_blocks);
                return 0;
            }
            has_default = 1;
            default_block = LLVMAppendBasicBlock(codegen->current_function, "select_default");
            case_blocks[case_index] = default_block;
        } else {
            // Regular case
            char case_name[32];
            snprintf(case_name, sizeof(case_name), "select_case_%zu", case_index);
            case_blocks[case_index] = LLVMAppendBasicBlock(codegen->current_function, case_name);
            
            // Setup select case data
            if (!codegen_setup_select_case(codegen, checker, cases_array, case_index, select_case)) {
                free(case_blocks);
                return 0;
            }
        }
        
        case_node = case_node->next;
        case_index++;
    }
    
    // Call goo_select to determine which case is ready
    LLVMValueRef select_func = codegen_get_select_function(codegen);
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMTypeRef param_types[] = {
        void_ptr_type,   // goo_select_case_t* cases
        LLVMInt64Type(), // size_t num_cases
        LLVMInt64Type()  // int64_t timeout_ns
    };
    LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32Type(), param_types, 3, 0);
    
    LLVMValueRef case_count_val = LLVMConstInt(LLVMInt64Type(), case_count - (has_default ? 1 : 0), 0);
    LLVMValueRef timeout_val = LLVMConstInt(LLVMInt64Type(), -1, 0); // No timeout
    
    LLVMValueRef args[] = { cases_array, case_count_val, timeout_val };
    LLVMValueRef selected_case = LLVMBuildCall2(codegen->builder, func_type, select_func, args, 3, "selected_case");
    
    // Create switch based on the result
    LLVMValueRef switch_inst = LLVMBuildSwitch(codegen->builder, selected_case, 
                                               has_default ? default_block : end_block, 
                                               (unsigned)(case_count - (has_default ? 1 : 0)));
    
    // Add cases to switch
    case_index = 0;
    case_node = select_stmt->cases;
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;
        
        if (select_case->comm != NULL) {
            // Regular case - add to switch
            LLVMValueRef case_val = LLVMConstInt(LLVMInt32Type(), case_index, 0);
            LLVMAddCase(switch_inst, case_val, case_blocks[case_index]);
        }
        
        case_node = case_node->next;
        case_index++;
    }
    
    // Generate code for each case block
    case_index = 0;
    case_node = select_stmt->cases;
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;
        
        LLVMPositionBuilderAtEnd(codegen->builder, case_blocks[case_index]);
        
        // Generate case body
        if (select_case->body) {
            if (!codegen_generate_statement(codegen, checker, select_case->body)) {
                free(case_blocks);
                return 0;
            }
        }
        
        // Branch to end
        LLVMBuildBr(codegen->builder, end_block);
        
        case_node = case_node->next;
        case_index++;
    }
    
    // Position builder at end block
    LLVMPositionBuilderAtEnd(codegen->builder, end_block);
    
    free(case_blocks);
    return 1;
#endif
}

#if LLVM_AVAILABLE
// Helper function to get select case type
LLVMTypeRef codegen_get_select_case_type(CodeGenerator* codegen __attribute__((unused))) {
    // Create struct type for goo_select_case_t
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMTypeRef field_types[] = {
        void_ptr_type,  // goo_channel_t* channel
        void_ptr_type,  // void* data
        LLVMInt32Type(), // int is_send
        LLVMInt32Type()  // int ready
    };
    return LLVMStructType(field_types, 4, 0);
}
#endif

#if LLVM_AVAILABLE
// Helper function to get goo_select function
LLVMValueRef codegen_get_select_function(CodeGenerator* codegen) {
    LLVMValueRef select_func = LLVMGetNamedFunction(codegen->module, "goo_select");
    if (!select_func) {
        // Declare goo_select if not already declared
        LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
        LLVMTypeRef param_types[] = {
            void_ptr_type,   // goo_select_case_t* cases
            LLVMInt64Type(), // size_t num_cases
            LLVMInt64Type()  // int64_t timeout_ns
        };
        LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32Type(), param_types, 3, 0);
        select_func = LLVMAddFunction(codegen->module, "goo_select", func_type);
    }
    return select_func;
}
#endif

#if LLVM_AVAILABLE
// Helper function to setup select case data
int codegen_setup_select_case(CodeGenerator* codegen, TypeChecker* checker, 
                              LLVMValueRef cases_array, size_t case_index, 
                              SelectCaseNode* select_case) {
    // Get pointer to the case struct in the array
    LLVMTypeRef select_case_type = codegen_get_select_case_type(codegen);
    LLVMValueRef indices[] = {
        LLVMConstInt(LLVMInt32Type(), 0, 0),           // Array index
        LLVMConstInt(LLVMInt32Type(), case_index, 0)   // Case index
    };
    LLVMValueRef case_ptr = LLVMBuildGEP2(codegen->builder, select_case_type, cases_array, indices, 2, "case_ptr");
    
    // Parse the communication operation
    if (!select_case->comm) return 0;
    
    LLVMValueRef channel = NULL;
    LLVMValueRef data_ptr = NULL;
    int is_send = 0;
    
    // Determine if this is a send or receive operation
    if (select_case->comm->type == AST_BINARY_EXPR) {
        // Channel send: ch <- value
        BinaryExprNode* binary = (BinaryExprNode*)select_case->comm;
        if (binary->operator == TOKEN_ARROW) {
            is_send = 1;
            
            // Generate channel and value
            ValueInfo* channel_val = codegen_generate_expression(codegen, checker, binary->left);
            if (!channel_val) return 0;
            channel = channel_val->llvm_value;
            
            ValueInfo* value_val = codegen_generate_expression(codegen, checker, binary->right);
            if (!value_val) return 0;
            
            // Get pointer to the value
            if (value_val->is_lvalue) {
                data_ptr = value_val->llvm_value;
            } else {
                // Store value temporarily
                LLVMValueRef temp_alloca = LLVMBuildAlloca(codegen->builder, 
                                                          LLVMTypeOf(value_val->llvm_value), 
                                                          "temp_send_value");
                LLVMBuildStore(codegen->builder, value_val->llvm_value, temp_alloca);
                data_ptr = temp_alloca;
            }
            
            value_info_free(channel_val);
            value_info_free(value_val);
        }
    } else if (select_case->comm->type == AST_UNARY_EXPR) {
        // Channel receive: <-ch
        UnaryExprNode* unary = (UnaryExprNode*)select_case->comm;
        if (unary->operator == TOKEN_ARROW) {
            is_send = 0;
            
            // Generate channel
            ValueInfo* channel_val = codegen_generate_expression(codegen, checker, unary->operand);
            if (!channel_val) return 0;
            channel = channel_val->llvm_value;
            
            // Allocate space for received value
            data_ptr = LLVMBuildAlloca(codegen->builder, LLVMInt32Type(), "recv_space");
            
            value_info_free(channel_val);
        }
    }
    
    if (!channel || !data_ptr) {
        codegen_error(codegen, select_case->comm->pos, "Invalid channel operation in select case");
        return 0;
    }
    
    // Cast pointers to void*
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    channel = LLVMBuildBitCast(codegen->builder, channel, void_ptr_type, "channel_void_ptr");
    data_ptr = LLVMBuildBitCast(codegen->builder, data_ptr, void_ptr_type, "data_void_ptr");
    
    // Set the case fields
    // case_ptr->channel = channel
    LLVMValueRef channel_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 0, "channel_field");
    LLVMBuildStore(codegen->builder, channel, channel_field_ptr);
    
    // case_ptr->data = data_ptr
    LLVMValueRef data_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 1, "data_field");
    LLVMBuildStore(codegen->builder, data_ptr, data_field_ptr);
    
    // case_ptr->is_send = is_send
    LLVMValueRef is_send_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 2, "is_send_field");
    LLVMValueRef is_send_val = LLVMConstInt(LLVMInt1Type(), is_send, 0);
    LLVMBuildStore(codegen->builder, is_send_val, is_send_field_ptr);
    
    return 1;
}

// Unsafe statement generation
int codegen_generate_unsafe_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_UNSAFE_STMT) return 0;
    
    UnsafeStmtNode* unsafe_stmt = (UnsafeStmtNode*)stmt;
    
    // For now, unsafe blocks are just transparent - they contain the actual unsafe operations
    // In the future, we might want to add runtime checks or metadata here
    
    // Generate the body of the unsafe block
    return codegen_generate_statement(codegen, checker, unsafe_stmt->body);
#endif
}

// Inline assembly statement generation
int codegen_generate_asm_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_ASM_STMT) return 0;
    
    AsmStmtNode* asm_stmt = (AsmStmtNode*)stmt;
    
    // Create inline assembly function type (void -> void for now)
    LLVMTypeRef func_type = LLVMFunctionType(LLVMVoidType(), NULL, 0, 0);
    
    // Create inline assembly with the provided assembly code
    const char* constraints = "~{dirflag},~{fpsr},~{flags}"; // Basic x86 clobbers
    
    LLVMValueRef inline_asm = LLVMGetInlineAsm(func_type, 
                                               asm_stmt->assembly_code, strlen(asm_stmt->assembly_code),
                                               (char*)constraints, strlen(constraints),
                                               1, 1, LLVMInlineAsmDialectIntel, 0);
    
    // Call the inline assembly
    LLVMBuildCall2(codegen->builder, func_type, inline_asm, NULL, 0, "inline_asm");
    
    return 1;
#endif
}
#endif