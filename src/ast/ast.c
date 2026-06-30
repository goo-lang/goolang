#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// AST node type to string mapping
static const char* ast_node_type_strings[] = {
    [AST_PROGRAM] = "Program",
    [AST_PACKAGE_DECL] = "PackageDecl",
    [AST_IMPORT_SPEC] = "ImportSpec",
    [AST_FUNC_DECL] = "FuncDecl",
    [AST_VAR_DECL] = "VarDecl",
    [AST_CONST_DECL] = "ConstDecl",
    [AST_TYPE_DECL] = "TypeDecl",
    [AST_CONCEPT_DECL] = "ConceptDecl",
    
    [AST_BLOCK_STMT] = "BlockStmt",
    [AST_EXPR_STMT] = "ExprStmt",
    [AST_IF_STMT] = "IfStmt",
    [AST_IF_LET_STMT] = "IfLetStmt",
    [AST_FOR_STMT] = "ForStmt",
    [AST_RETURN_STMT] = "ReturnStmt",
    [AST_BREAK_STMT] = "BreakStmt",
    [AST_CONTINUE_STMT] = "ContinueStmt",
    [AST_DEFER_STMT] = "DeferStmt",
    [AST_GO_STMT] = "GoStmt",
    [AST_SELECT_STMT] = "SelectStmt",
    [AST_SELECT_CASE] = "SelectCase",
    [AST_SWITCH_STMT] = "SwitchStmt",
    [AST_CASE_CLAUSE] = "CaseClause",
    [AST_DEFAULT_CLAUSE] = "DefaultClause",
    [AST_UNSAFE_STMT] = "UnsafeStmt",
    [AST_ASM_STMT] = "AsmStmt",
    
    [AST_IDENTIFIER] = "Identifier",
    [AST_LITERAL] = "Literal",
    [AST_BINARY_EXPR] = "BinaryExpr",
    [AST_UNARY_EXPR] = "UnaryExpr",
    [AST_POSTFIX_EXPR] = "PostfixExpr",
    [AST_CALL_EXPR] = "CallExpr",
    [AST_INDEX_EXPR] = "IndexExpr",
    [AST_SLICE_INDEX_EXPR] = "SliceIndexExpr",
    [AST_SELECTOR_EXPR] = "SelectorExpr",
    [AST_SLICE_EXPR] = "SliceExpr",
    [AST_TYPE_ASSERT_EXPR] = "TypeAssertExpr",
    [AST_PAREN_EXPR] = "ParenExpr",
    [AST_STRUCT_LITERAL] = "StructLiteral",

    [AST_BASIC_TYPE] = "BasicType",
    [AST_ARRAY_TYPE] = "ArrayType",
    [AST_SLICE_TYPE] = "SliceType",
    [AST_MAP_TYPE] = "MapType",
    [AST_CHAN_TYPE] = "ChanType",
    [AST_FUNC_TYPE] = "FuncType",
    [AST_INTERFACE_TYPE] = "InterfaceType",
    [AST_STRUCT_TYPE] = "StructType",
    [AST_POINTER_TYPE] = "PointerType",
    [AST_REFERENCE_TYPE] = "ReferenceType",
    
    [AST_ERROR_UNION_TYPE] = "ErrorUnionType",
    [AST_NULLABLE_TYPE] = "NullableType",
    [AST_TRY_EXPR] = "TryExpr",
    [AST_CATCH_EXPR] = "CatchExpr",
    [AST_COMPTIME_BLOCK] = "ComptimeBlock",
    [AST_OWNERSHIP_QUAL] = "OwnershipQual",
    [AST_UNSAFE_PTR_TYPE] = "UnsafePtrType",
    [AST_PTR_ARITHMETIC] = "PtrArithmetic",
    [AST_PTR_DEREF] = "PtrDeref",
    [AST_ADDR_OF] = "AddrOf",
    [AST_PORT_IO] = "PortIO",
    [AST_MMIO_ACCESS] = "MMIOAccess",
    [AST_EXTERN_DECL] = "ExternDecl",
    [AST_ATTRIBUTE] = "Attribute",
    [AST_VOLATILE_EXPR] = "VolatileExpr",
    [AST_PARALLEL_FOR] = "ParallelFor",
    [AST_PARALLEL_REDUCE] = "ParallelReduce",
    [AST_BARRIER_CALL] = "BarrierCall",
    [AST_ATOMIC_EXPR] = "AtomicExpr",
    [AST_THREAD_LOCAL_DECL] = "ThreadLocalDecl",
    [AST_MATCH_EXPR] = "MatchExpr",
    [AST_MATCH_CASE] = "MatchCase",
    [AST_PATTERN] = "Pattern",
    [AST_GUARD_CONDITION] = "GuardCondition",
    [AST_KERNEL_DECL] = "KernelDecl",
    [AST_KERNEL_LAUNCH] = "KernelLaunch",
    [AST_GPU_MEMORY_ALLOC] = "GPUMemoryAlloc",
    [AST_GPU_MEMORY_COPY] = "GPUMemoryCopy",
    [AST_GPU_SYNC] = "GPUSync",
    [AST_GPU_INTRINSIC] = "GPUIntrinsic",
    [AST_WASM_EXPORT] = "WasmExport",
    [AST_WASM_IMPORT] = "WasmImport",
    [AST_WASM_MEMORY] = "WasmMemory",
    [AST_WASM_TABLE] = "WasmTable",
    [AST_WASM_GLOBAL] = "WasmGlobal",
    [AST_WASM_TYPE] = "WasmType",
    [AST_WASM_START] = "WasmStart",
    [AST_WASM_ELEM] = "WasmElem",
    [AST_WASM_DATA] = "WasmData",
    [AST_JS_INTEROP] = "JSInterop",
    [AST_DOM_ACCESS] = "DOMAccess",
};

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

// Base AST node creation
ASTNode* ast_node_new(ASTNodeType type, Position pos) {
    ASTNode* node = malloc(sizeof(ASTNode));
    if (!node) return NULL;
    
    node->type = type;
    node->pos = pos;
    node->node_type = NULL;
    node->next = NULL;
    
    return node;
}

// Generic node destruction (needs to be extended for specific types)
void ast_node_free(ASTNode* node) {
    if (!node) return;
    
    // Free type-specific data
    switch (node->type) {
        case AST_PROGRAM: {
            ProgramNode* prog = (ProgramNode*)node;
            free(prog->package_name);
            ast_node_free(prog->imports);
            ast_node_free(prog->decls);
            break;
        }
        case AST_PACKAGE_DECL: {
            PackageDeclNode* pkg = (PackageDeclNode*)node;
            free(pkg->name);
            break;
        }
        case AST_IMPORT_SPEC: {
            ImportSpecNode* imp = (ImportSpecNode*)node;
            free(imp->path);
            free(imp->alias);
            break;
        }
        case AST_FUNC_DECL: {
            FuncDeclNode* func = (FuncDeclNode*)node;
            free(func->name);
            ast_node_free(func->params);
            ast_node_free(func->return_type);
            ast_node_free(func->body);
            ast_node_free(func->annotations);
            break;
        }
        case AST_CONCEPT_DECL: {
            ConceptDeclNode* concept = (ConceptDeclNode*)node;
            free(concept->name);
            ast_node_free(concept->type_params);
            ast_node_free(concept->requirements);
            break;
        }
        case AST_VAR_DECL: {
            VarDeclNode* var = (VarDeclNode*)node;
            for (size_t i = 0; i < var->name_count; i++) {
                free(var->names[i]);
            }
            free(var->names);
            ast_node_free(var->type);
            ast_node_free(var->values);
            break;
        }
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)node;
            free(ident->name);
            break;
        }
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)node;
            free(lit->value);
            break;
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)node;
            ast_node_free(binary->left);
            ast_node_free(binary->right);
            break;
        }
        case AST_STRUCT_LITERAL: {
            StructLiteralNode* lit = (StructLiteralNode*)node;
            free(lit->type_name);
            if (lit->field_names) {
                for (size_t i = 0; i < lit->field_count; i++) {
                    free(lit->field_names[i]);
                }
                free(lit->field_names);
            }
            ast_node_free(lit->field_values);
            break;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)node;
            ast_node_free(unary->operand);
            break;
        }
        case AST_POSTFIX_EXPR: {
            PostfixExprNode* postfix = (PostfixExprNode*)node;
            ast_node_free(postfix->operand);
            break;
        }
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)node;
            ast_node_free(block->statements);
            break;
        }
        case AST_IF_LET_STMT: {
            IfLetStmtNode* if_let = (IfLetStmtNode*)node;
            free(if_let->var_name);
            ast_node_free(if_let->nullable_expr);
            ast_node_free(if_let->then_stmt);
            ast_node_free(if_let->else_stmt);
            break;
        }
        case AST_ERROR_UNION_TYPE: {
            ErrorUnionTypeNode* error_union = (ErrorUnionTypeNode*)node;
            ast_node_free(error_union->value_type);
            ast_node_free(error_union->error_type);
            break;
        }
        case AST_NULLABLE_TYPE: {
            NullableTypeNode* nullable = (NullableTypeNode*)node;
            ast_node_free(nullable->base_type);
            break;
        }
        case AST_TRY_EXPR: {
            TryExprNode* try_expr = (TryExprNode*)node;
            ast_node_free(try_expr->expr);
            break;
        }
        case AST_CATCH_EXPR: {
            CatchExprNode* catch_expr = (CatchExprNode*)node;
            ast_node_free(catch_expr->expr);
            free(catch_expr->error_var);
            ast_node_free(catch_expr->catch_body);
            break;
        }
        case AST_COMPTIME_BLOCK: {
            ComptimeBlockNode* comptime_block = (ComptimeBlockNode*)node;
            ast_node_free(comptime_block->body);
            break;
        }
        case AST_CHAN_TYPE: {
            ChanTypeNode* chan = (ChanTypeNode*)node;
            ast_node_free(chan->element_type);
            free(chan->endpoint);
            break;
        }
        case AST_REFERENCE_TYPE: {
            ReferenceTypeNode* ref = (ReferenceTypeNode*)node;
            ast_node_free(ref->element_type);
            break;
        }
        case AST_UNSAFE_STMT: {
            UnsafeStmtNode* unsafe_stmt = (UnsafeStmtNode*)node;
            ast_node_free(unsafe_stmt->body);
            break;
        }
        case AST_ASM_STMT: {
            AsmStmtNode* asm_stmt = (AsmStmtNode*)node;
            free(asm_stmt->assembly_code);
            ast_node_free(asm_stmt->outputs);
            ast_node_free(asm_stmt->inputs);
            for (size_t i = 0; i < asm_stmt->clobber_count; i++) {
                free(asm_stmt->clobbers[i]);
            }
            free(asm_stmt->clobbers);
            break;
        }
        case AST_UNSAFE_PTR_TYPE: {
            UnsafePtrTypeNode* unsafe_ptr = (UnsafePtrTypeNode*)node;
            ast_node_free(unsafe_ptr->element_type);
            break;
        }
        case AST_PTR_ARITHMETIC: {
            PtrArithmeticNode* ptr_arith = (PtrArithmeticNode*)node;
            ast_node_free(ptr_arith->pointer);
            ast_node_free(ptr_arith->offset);
            break;
        }
        case AST_PTR_DEREF: {
            PtrDerefNode* ptr_deref = (PtrDerefNode*)node;
            ast_node_free(ptr_deref->pointer);
            break;
        }
        case AST_ADDR_OF: {
            AddrOfNode* addr_of = (AddrOfNode*)node;
            ast_node_free(addr_of->operand);
            break;
        }
        case AST_PORT_IO: {
            PortIONode* port_io = (PortIONode*)node;
            ast_node_free(port_io->port);
            ast_node_free(port_io->value);
            break;
        }
        case AST_MMIO_ACCESS: {
            MMIOAccessNode* mmio = (MMIOAccessNode*)node;
            ast_node_free(mmio->address);
            ast_node_free(mmio->value);
            break;
        }
        case AST_EXTERN_DECL: {
            ExternDeclNode* extern_decl = (ExternDeclNode*)node;
            free(extern_decl->name);
            free(extern_decl->abi);
            free(extern_decl->library);
            ast_node_free(extern_decl->params);
            ast_node_free(extern_decl->return_type);
            break;
        }
        case AST_ATTRIBUTE: {
            AttributeNode* attr = (AttributeNode*)node;
            free(attr->name);
            ast_node_free(attr->args);
            break;
        }
        case AST_VOLATILE_EXPR: {
            VolatileExprNode* volatile_expr = (VolatileExprNode*)node;
            ast_node_free(volatile_expr->expr);
            break;
        }
        case AST_PARALLEL_FOR: {
            ParallelForNode* parallel_for = (ParallelForNode*)node;
            ast_node_free(parallel_for->init);
            ast_node_free(parallel_for->condition);
            ast_node_free(parallel_for->increment);
            ast_node_free(parallel_for->body);
            free(parallel_for->schedule_type);
            break;
        }
        case AST_PARALLEL_REDUCE: {
            ParallelReduceNode* parallel_reduce = (ParallelReduceNode*)node;
            ast_node_free(parallel_reduce->array);
            ast_node_free(parallel_reduce->init_value);
            ast_node_free(parallel_reduce->reduction_func);
            free(parallel_reduce->operation);
            break;
        }
        case AST_BARRIER_CALL: {
            BarrierCallNode* barrier_call = (BarrierCallNode*)node;
            free(barrier_call->barrier_name);
            break;
        }
        case AST_ATOMIC_EXPR: {
            AtomicExprNode* atomic_expr = (AtomicExprNode*)node;
            ast_node_free(atomic_expr->expr);
            ast_node_free(atomic_expr->operand);
            free(atomic_expr->operation);
            break;
        }
        case AST_THREAD_LOCAL_DECL: {
            ThreadLocalDeclNode* thread_local_node = (ThreadLocalDeclNode*)node;
            free(thread_local_node->name);
            ast_node_free(thread_local_node->type);
            ast_node_free(thread_local_node->init_value);
            break;
        }
        case AST_MATCH_EXPR: {
            MatchExprNode* match_expr = (MatchExprNode*)node;
            ast_node_free(match_expr->expr);
            ast_node_free(match_expr->cases);
            break;
        }
        case AST_MATCH_CASE: {
            MatchCaseNode* match_case = (MatchCaseNode*)node;
            ast_node_free(match_case->pattern);
            ast_node_free(match_case->guard);
            ast_node_free(match_case->body);
            break;
        }
        case AST_PATTERN: {
            PatternNode* pattern = (PatternNode*)node;
            switch (pattern->pattern_type) {
                case PATTERN_LITERAL:
                    ast_node_free(pattern->data.literal.literal);
                    break;
                case PATTERN_IDENTIFIER:
                    free(pattern->data.identifier.name);
                    ast_node_free(pattern->data.identifier.type);
                    break;
                case PATTERN_DESTRUCTURE:
                    free(pattern->data.destructure.type_name);
                    ast_node_free(pattern->data.destructure.fields);
                    break;
                case PATTERN_OR:
                    ast_node_free(pattern->data.or_pattern.patterns);
                    break;
                default:
                    break;
            }
            break;
        }
        case AST_GUARD_CONDITION: {
            GuardConditionNode* guard = (GuardConditionNode*)node;
            ast_node_free(guard->condition);
            break;
        }
        case AST_KERNEL_DECL: {
            KernelDeclNode* kernel = (KernelDeclNode*)node;
            free(kernel->name);
            ast_node_free(kernel->params);
            ast_node_free(kernel->return_type);
            ast_node_free(kernel->body);
            break;
        }
        case AST_KERNEL_LAUNCH: {
            KernelLaunchNode* launch = (KernelLaunchNode*)node;
            ast_node_free(launch->kernel_func);
            ast_node_free(launch->grid_dim);
            ast_node_free(launch->block_dim);
            ast_node_free(launch->args);
            ast_node_free(launch->shared_mem_size);
            ast_node_free(launch->stream);
            break;
        }
        case AST_GPU_MEMORY_ALLOC: {
            GPUMemoryAllocNode* alloc = (GPUMemoryAllocNode*)node;
            ast_node_free(alloc->size);
            ast_node_free(alloc->element_type);
            break;
        }
        case AST_GPU_MEMORY_COPY: {
            GPUMemoryCopyNode* copy = (GPUMemoryCopyNode*)node;
            ast_node_free(copy->dest);
            ast_node_free(copy->src);
            ast_node_free(copy->size);
            ast_node_free(copy->stream);
            break;
        }
        case AST_GPU_SYNC: {
            GPUSyncNode* sync = (GPUSyncNode*)node;
            ast_node_free(sync->stream);
            ast_node_free(sync->event);
            break;
        }
        case AST_GPU_INTRINSIC: {
            GPUIntrinsicNode* intrinsic = (GPUIntrinsicNode*)node;
            free(intrinsic->intrinsic_name);
            ast_node_free(intrinsic->args);
            break;
        }
        case AST_WASM_EXPORT: {
            WasmExportNode* wasm_export = (WasmExportNode*)node;
            free(wasm_export->export_name);
            free(wasm_export->export_type);
            ast_node_free(wasm_export->item);
            break;
        }
        case AST_WASM_IMPORT: {
            WasmImportNode* wasm_import = (WasmImportNode*)node;
            free(wasm_import->module_name);
            free(wasm_import->import_name);
            free(wasm_import->local_name);
            free(wasm_import->import_type);
            ast_node_free(wasm_import->signature);
            break;
        }
        case AST_WASM_MEMORY: {
            WasmMemoryNode* wasm_memory = (WasmMemoryNode*)node;
            ast_node_free(wasm_memory->min_pages);
            ast_node_free(wasm_memory->max_pages);
            free(wasm_memory->export_name);
            break;
        }
        case AST_WASM_TABLE: {
            WasmTableNode* wasm_table = (WasmTableNode*)node;
            ast_node_free(wasm_table->min_size);
            ast_node_free(wasm_table->max_size);
            free(wasm_table->export_name);
            break;
        }
        case AST_WASM_GLOBAL: {
            WasmGlobalNode* wasm_global = (WasmGlobalNode*)node;
            free(wasm_global->name);
            free(wasm_global->export_name);
            ast_node_free(wasm_global->init_value);
            break;
        }
        case AST_WASM_TYPE: {
            WasmTypeNode* wasm_type = (WasmTypeNode*)node;
            free(wasm_type->name);
            ast_node_free(wasm_type->params);
            ast_node_free(wasm_type->results);
            break;
        }
        case AST_WASM_START: {
            WasmStartNode* wasm_start = (WasmStartNode*)node;
            ast_node_free(wasm_start->function);
            break;
        }
        case AST_WASM_ELEM: {
            WasmElemNode* wasm_elem = (WasmElemNode*)node;
            ast_node_free(wasm_elem->table_index);
            ast_node_free(wasm_elem->offset);
            ast_node_free(wasm_elem->elements);
            break;
        }
        case AST_WASM_DATA: {
            WasmDataNode* wasm_data = (WasmDataNode*)node;
            ast_node_free(wasm_data->memory_index);
            ast_node_free(wasm_data->offset);
            ast_node_free(wasm_data->data);
            break;
        }
        case AST_JS_INTEROP: {
            JSInteropNode* js_interop = (JSInteropNode*)node;
            free(js_interop->object_name);
            free(js_interop->property_name);
            ast_node_free(js_interop->args);
            break;
        }
        case AST_DOM_ACCESS: {
            DOMAccessNode* dom_access = (DOMAccessNode*)node;
            free(dom_access->api_name);
            free(dom_access->method_name);
            ast_node_free(dom_access->args);
            break;
        }
        case AST_SWITCH_STMT: {
            SwitchStmtNode* sw = (SwitchStmtNode*)node;
            ast_node_free(sw->tag);
            ast_node_free(sw->cases);
            break;
        }
        case AST_CASE_CLAUSE: {
            CaseClauseNode* clause = (CaseClauseNode*)node;
            ast_node_free(clause->exprs);
            ast_node_free(clause->body);
            break;
        }
        // Add more cases as needed
        default:
            break;
    }
    
    // Free the next node in the list
    if (node->next) {
        ast_node_free(node->next);
    }
    
    free(node);
}

// Deep-clone a *type* AST node (the type-expression node kinds only).
//
// Used by the parser to expand a grouped named result list `(x, y int)` into
// one VarDecl per name: each name needs its OWN copy of the shared type node so
// AST teardown (ast_node_free walks each VarDecl's ->type) frees it exactly
// once. Sharing a single type pointer across VarDecls would double-free.
//
// Covers the type kinds reachable as a grouped-result element type; returns
// NULL for kinds the grouped-name path does not need to duplicate (array — its
// length is an expression; struct/enum/func types), letting the caller fall
// back to the prior behavior for those rare shapes rather than mis-cloning.
ASTNode* ast_type_clone(const ASTNode* node) {
    if (!node) return NULL;
    switch (node->type) {
        case AST_BASIC_TYPE: {
            const BasicTypeNode* s = (const BasicTypeNode*)node;
            BasicTypeNode* c = (BasicTypeNode*)calloc(1, sizeof(BasicTypeNode));
            c->base.type = AST_BASIC_TYPE; c->base.pos = node->pos;
            c->name = s->name ? strdup(s->name) : NULL;
            return (ASTNode*)c;
        }
        case AST_SLICE_TYPE: {
            const SliceTypeNode* s = (const SliceTypeNode*)node;
            SliceTypeNode* c = (SliceTypeNode*)calloc(1, sizeof(SliceTypeNode));
            c->base.type = AST_SLICE_TYPE; c->base.pos = node->pos;
            c->element_type = ast_type_clone(s->element_type);
            return (ASTNode*)c;
        }
        case AST_MAP_TYPE: {
            const MapTypeNode* s = (const MapTypeNode*)node;
            MapTypeNode* c = (MapTypeNode*)calloc(1, sizeof(MapTypeNode));
            c->base.type = AST_MAP_TYPE; c->base.pos = node->pos;
            c->key_type = ast_type_clone(s->key_type);
            c->value_type = ast_type_clone(s->value_type);
            return (ASTNode*)c;
        }
        case AST_CHAN_TYPE: {
            const ChanTypeNode* s = (const ChanTypeNode*)node;
            ChanTypeNode* c = (ChanTypeNode*)calloc(1, sizeof(ChanTypeNode));
            c->base.type = AST_CHAN_TYPE; c->base.pos = node->pos;
            c->element_type = ast_type_clone(s->element_type);
            c->pattern = s->pattern;
            c->endpoint = s->endpoint ? strdup(s->endpoint) : NULL;
            return (ASTNode*)c;
        }
        case AST_POINTER_TYPE: {
            const PointerTypeNode* s = (const PointerTypeNode*)node;
            PointerTypeNode* c = (PointerTypeNode*)calloc(1, sizeof(PointerTypeNode));
            c->base.type = AST_POINTER_TYPE; c->base.pos = node->pos;
            c->element_type = ast_type_clone(s->element_type);
            return (ASTNode*)c;
        }
        case AST_REFERENCE_TYPE: {
            const ReferenceTypeNode* s = (const ReferenceTypeNode*)node;
            ReferenceTypeNode* c = (ReferenceTypeNode*)calloc(1, sizeof(ReferenceTypeNode));
            c->base.type = AST_REFERENCE_TYPE; c->base.pos = node->pos;
            c->element_type = ast_type_clone(s->element_type);
            c->is_mutable = s->is_mutable;
            return (ASTNode*)c;
        }
        case AST_UNSAFE_PTR_TYPE: {
            const UnsafePtrTypeNode* s = (const UnsafePtrTypeNode*)node;
            UnsafePtrTypeNode* c = (UnsafePtrTypeNode*)calloc(1, sizeof(UnsafePtrTypeNode));
            c->base.type = AST_UNSAFE_PTR_TYPE; c->base.pos = node->pos;
            c->element_type = ast_type_clone(s->element_type);
            return (ASTNode*)c;
        }
        case AST_NULLABLE_TYPE: {
            const NullableTypeNode* s = (const NullableTypeNode*)node;
            NullableTypeNode* c = (NullableTypeNode*)calloc(1, sizeof(NullableTypeNode));
            c->base.type = AST_NULLABLE_TYPE; c->base.pos = node->pos;
            c->base_type = ast_type_clone(s->base_type);
            return (ASTNode*)c;
        }
        case AST_ERROR_UNION_TYPE: {
            const ErrorUnionTypeNode* s = (const ErrorUnionTypeNode*)node;
            ErrorUnionTypeNode* c = (ErrorUnionTypeNode*)calloc(1, sizeof(ErrorUnionTypeNode));
            c->base.type = AST_ERROR_UNION_TYPE; c->base.pos = node->pos;
            c->value_type = ast_type_clone(s->value_type);
            c->error_type = ast_type_clone(s->error_type);
            return (ASTNode*)c;
        }
        default:
            return NULL;
    }
}

// Specific node constructors
void ast_add_child(ASTNode* parent, ASTNode* child) {
    if (!parent || !child) return;
    
    if (!parent->next) {
        parent->next = child;
    } else {
        ASTNode* current = parent->next;
        while (current->next) {
            current = current->next;
        }
        current->next = child;
    }
}

const char* ast_node_type_string(ASTNodeType type) {
    if (type >= 0 && type < AST_NODE_COUNT) {
        return ast_node_type_strings[type];
    }
    return "Unknown";
}

void ast_print(const ASTNode* node, int indent) {
    if (!node) return;
    
    // Print indentation
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    printf("%s", ast_node_type_string(node->type));
    
    // Print type-specific information
    switch (node->type) {
        case AST_IDENTIFIER: {
            const IdentifierNode* ident = (const IdentifierNode*)node;
            printf(" (%s)", ident->name ? ident->name : "NULL");
            break;
        }
        case AST_LITERAL: {
            const LiteralNode* lit = (const LiteralNode*)node;
            printf(" (%s: %s)", token_type_string(lit->literal_type), 
                   lit->value ? lit->value : "NULL");
            break;
        }
        case AST_BINARY_EXPR: {
            const BinaryExprNode* binary = (const BinaryExprNode*)node;
            printf(" (%s)", token_type_string(binary->operator));
            break;
        }
        case AST_UNARY_EXPR: {
            const UnaryExprNode* unary = (const UnaryExprNode*)node;
            printf(" (%s)", token_type_string(unary->operator));
            break;
        }
        default:
            break;
    }
    
    printf(" [%d:%d]\n", node->pos.line, node->pos.column);
    
    // Print children (this is a simplified version)
    switch (node->type) {
        case AST_BINARY_EXPR: {
            const BinaryExprNode* binary = (const BinaryExprNode*)node;
            ast_print(binary->left, indent + 1);
            ast_print(binary->right, indent + 1);
            break;
        }
        case AST_UNARY_EXPR: {
            const UnaryExprNode* unary = (const UnaryExprNode*)node;
            ast_print(unary->operand, indent + 1);
            break;
        }
        case AST_BLOCK_STMT: {
            const BlockStmtNode* block = (const BlockStmtNode*)node;
            ast_print(block->statements, indent + 1);
            break;
        }
        default:
            break;
    }
    
    // Print next node in list
    if (node->next) {
        ast_print(node->next, indent);
    }
}

// Deep copy an AST node
ASTNode* ast_node_copy(const ASTNode* node) {
    if (!node) return NULL;
    
    // Create new node of same type
    ASTNode* copy = ast_node_new(node->type, ((ASTNode*)node)->pos);
    if (!copy) return NULL;
    
    // Copy common fields
    copy->next = ast_node_copy(node->next);
    
    // Copy type-specific data (simplified for now)
    switch (node->type) {
        case AST_IDENTIFIER:
            ((IdentifierNode*)copy)->name = str_dup(((IdentifierNode*)node)->name);
            break;
            
        case AST_LITERAL:
            ((LiteralNode*)copy)->literal_type = ((LiteralNode*)node)->literal_type;
            ((LiteralNode*)copy)->value = str_dup(((LiteralNode*)node)->value);
            break;
            
        case AST_BINARY_EXPR:
            ((BinaryExprNode*)copy)->left = ast_node_copy(((BinaryExprNode*)node)->left);
            ((BinaryExprNode*)copy)->operator = ((BinaryExprNode*)node)->operator;
            ((BinaryExprNode*)copy)->right = ast_node_copy(((BinaryExprNode*)node)->right);
            break;
            
        case AST_UNARY_EXPR:
            ((UnaryExprNode*)copy)->operator = ((UnaryExprNode*)node)->operator;
            ((UnaryExprNode*)copy)->operand = ast_node_copy(((UnaryExprNode*)node)->operand);
            break;
            
        // Add more cases as needed
        default:
            // For complex nodes, just copy the base structure
            break;
    }
    
    return copy;
}
