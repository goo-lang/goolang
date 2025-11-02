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
    [AST_SELECTOR_EXPR] = "SelectorExpr",
    [AST_SLICE_EXPR] = "SliceExpr",
    [AST_TYPE_ASSERT_EXPR] = "TypeAssertExpr",
    [AST_PAREN_EXPR] = "ParenExpr",
    
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

// Specific node constructors

ProgramNode* ast_program_new(Position pos) {
    ProgramNode* node = (ProgramNode*)malloc(sizeof(ProgramNode));
    if (!node) return NULL;
    
    node->base.type = AST_PROGRAM;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->package_name = NULL;
    node->imports = NULL;
    node->decls = NULL;
    
    return node;
}

PackageDeclNode* ast_package_decl_new(const char* name, Position pos) {
    PackageDeclNode* node = (PackageDeclNode*)malloc(sizeof(PackageDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_PACKAGE_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    
    return node;
}

ImportSpecNode* ast_import_spec_new(const char* path, const char* alias, Position pos) {
    ImportSpecNode* node = (ImportSpecNode*)malloc(sizeof(ImportSpecNode));
    if (!node) return NULL;
    
    node->base.type = AST_IMPORT_SPEC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->path = str_dup(path);
    node->alias = str_dup(alias);
    
    return node;
}

FuncDeclNode* ast_func_decl_new(const char* name, Position pos) {
    FuncDeclNode* node = (FuncDeclNode*)malloc(sizeof(FuncDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_FUNC_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->params = NULL;
    node->return_type = NULL;
    node->body = NULL;
    node->is_comptime = 0;
    node->is_unsafe = 0;
    
    return node;
}

ConceptDeclNode* ast_concept_decl_new(const char* name, Position pos) {
    ConceptDeclNode* node = (ConceptDeclNode*)malloc(sizeof(ConceptDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_CONCEPT_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->type_params = NULL;
    node->requirements = NULL;
    
    return node;
}

VarDeclNode* ast_var_decl_new(Position pos) {
    VarDeclNode* node = (VarDeclNode*)malloc(sizeof(VarDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_VAR_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->names = NULL;
    node->name_count = 0;
    node->type = NULL;
    node->values = NULL;
    node->ownership = OWNERSHIP_NONE;
    node->is_short_decl = 0;
    
    return node;
}

IdentifierNode* ast_identifier_new(const char* name, Position pos) {
    IdentifierNode* node = (IdentifierNode*)malloc(sizeof(IdentifierNode));
    if (!node) return NULL;
    
    node->base.type = AST_IDENTIFIER;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    
    return node;
}

LiteralNode* ast_literal_new(TokenType type, const char* value, Position pos) {
    LiteralNode* node = (LiteralNode*)malloc(sizeof(LiteralNode));
    if (!node) return NULL;
    
    node->base.type = AST_LITERAL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->literal_type = type;
    node->value = str_dup(value);
    
    // Debug: print what we're creating
    printf("DEBUG: Created literal node with type %d at %s:%d:%d, value='%s'\n", 
           node->base.type, pos.filename ? pos.filename : "unknown", pos.line, pos.column, value ? value : "null");
    
    return node;
}

BinaryExprNode* ast_binary_expr_new(ASTNode* left, TokenType op, ASTNode* right, Position pos) {
    BinaryExprNode* node = (BinaryExprNode*)malloc(sizeof(BinaryExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_BINARY_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->left = left;
    node->operator = op;
    node->right = right;
    
    return node;
}

UnaryExprNode* ast_unary_expr_new(TokenType op, ASTNode* operand, Position pos) {
    UnaryExprNode* node = (UnaryExprNode*)malloc(sizeof(UnaryExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_UNARY_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->operator = op;
    node->operand = operand;
    
    return node;
}

PostfixExprNode* ast_postfix_expr_new(ASTNode* operand, TokenType op, Position pos) {
    PostfixExprNode* node = (PostfixExprNode*)malloc(sizeof(PostfixExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_POSTFIX_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->operand = operand;
    node->operator = op;
    
    return node;
}

BlockStmtNode* ast_block_stmt_new(Position pos) {
    BlockStmtNode* node = (BlockStmtNode*)malloc(sizeof(BlockStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_BLOCK_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->statements = NULL;
    
    return node;
}

IfLetStmtNode* ast_if_let_stmt_new(const char* var_name, ASTNode* nullable_expr, ASTNode* then_stmt, ASTNode* else_stmt, Position pos) {
    IfLetStmtNode* node = (IfLetStmtNode*)malloc(sizeof(IfLetStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_IF_LET_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    
    // Copy variable name
    if (var_name) {
        node->var_name = malloc(strlen(var_name) + 1);
        if (node->var_name) {
            strcpy(node->var_name, var_name);
        }
    } else {
        node->var_name = NULL;
    }
    
    node->nullable_expr = nullable_expr;
    node->then_stmt = then_stmt;
    node->else_stmt = else_stmt;
    
    return node;
}

GoStmtNode* ast_go_stmt_new(ASTNode* call, Position pos) {
    GoStmtNode* node = (GoStmtNode*)malloc(sizeof(GoStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_GO_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->call = call;
    
    return node;
}

SelectStmtNode* ast_select_stmt_new(Position pos) {
    SelectStmtNode* node = (SelectStmtNode*)malloc(sizeof(SelectStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_SELECT_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->cases = NULL;
    
    return node;
}

SelectCaseNode* ast_select_case_new(ASTNode* comm, ASTNode* body, Position pos) {
    SelectCaseNode* node = (SelectCaseNode*)malloc(sizeof(SelectCaseNode));
    if (!node) return NULL;
    
    node->base.type = AST_SELECT_CASE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->comm = comm;
    node->body = body;
    
    return node;
}

DeferStmtNode* ast_defer_stmt_new(ASTNode* call, Position pos) {
    DeferStmtNode* node = (DeferStmtNode*)malloc(sizeof(DeferStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_DEFER_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->call = call;
    
    return node;
}

// Goo extension constructors

ErrorUnionTypeNode* ast_error_union_type_new(ASTNode* value_type, Position pos) {
    ErrorUnionTypeNode* node = (ErrorUnionTypeNode*)malloc(sizeof(ErrorUnionTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_ERROR_UNION_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->value_type = value_type;
    node->error_type = NULL; // Default to standard error type
    
    return node;
}

NullableTypeNode* ast_nullable_type_new(ASTNode* base_type, Position pos) {
    NullableTypeNode* node = (NullableTypeNode*)malloc(sizeof(NullableTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_NULLABLE_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->base_type = base_type;
    
    return node;
}

TryExprNode* ast_try_expr_new(ASTNode* expr, Position pos) {
    TryExprNode* node = (TryExprNode*)malloc(sizeof(TryExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_TRY_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    
    return node;
}

CatchExprNode* ast_catch_expr_new(ASTNode* expr, const char* error_var, ASTNode* catch_body, Position pos) {
    CatchExprNode* node = (CatchExprNode*)malloc(sizeof(CatchExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_CATCH_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    node->error_var = str_dup(error_var);
    node->catch_body = catch_body;
    
    return node;
}

ComptimeBlockNode* ast_comptime_block_new(ASTNode* body, Position pos) {
    ComptimeBlockNode* node = (ComptimeBlockNode*)malloc(sizeof(ComptimeBlockNode));
    if (!node) return NULL;
    
    node->base.type = AST_COMPTIME_BLOCK;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->body = body;
    
    return node;
}

ChanTypeNode* ast_chan_type_new(ASTNode* element_type, ChannelPattern pattern, Position pos) {
    ChanTypeNode* node = (ChanTypeNode*)malloc(sizeof(ChanTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_CHAN_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->element_type = element_type;
    node->pattern = pattern;
    node->endpoint = NULL;
    
    return node;
}

ReferenceTypeNode* ast_reference_type_new(ASTNode* element_type, int is_mutable, Position pos) {
    ReferenceTypeNode* node = (ReferenceTypeNode*)malloc(sizeof(ReferenceTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_REFERENCE_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->element_type = element_type;
    node->is_mutable = is_mutable;
    
    return node;
}

UnsafeStmtNode* ast_unsafe_stmt_new(ASTNode* body, Position pos) {
    UnsafeStmtNode* node = (UnsafeStmtNode*)malloc(sizeof(UnsafeStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_UNSAFE_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->body = body;
    
    return node;
}

AsmStmtNode* ast_asm_stmt_new(const char* assembly_code, Position pos) {
    AsmStmtNode* node = (AsmStmtNode*)malloc(sizeof(AsmStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_ASM_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->assembly_code = str_dup(assembly_code);
    node->outputs = NULL;
    node->inputs = NULL;
    node->clobbers = NULL;
    node->clobber_count = 0;
    
    return node;
}

UnsafePtrTypeNode* ast_unsafe_ptr_type_new(ASTNode* element_type, Position pos) {
    UnsafePtrTypeNode* node = (UnsafePtrTypeNode*)malloc(sizeof(UnsafePtrTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_UNSAFE_PTR_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->element_type = element_type;
    
    return node;
}

PtrArithmeticNode* ast_ptr_arithmetic_new(ASTNode* pointer, TokenType operation, ASTNode* offset, Position pos) {
    PtrArithmeticNode* node = (PtrArithmeticNode*)malloc(sizeof(PtrArithmeticNode));
    if (!node) return NULL;
    
    node->base.type = AST_PTR_ARITHMETIC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->pointer = pointer;
    node->operation = operation;
    node->offset = offset;
    
    return node;
}

PtrDerefNode* ast_ptr_deref_new(ASTNode* pointer, Position pos) {
    PtrDerefNode* node = (PtrDerefNode*)malloc(sizeof(PtrDerefNode));
    if (!node) return NULL;
    
    node->base.type = AST_PTR_DEREF;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->pointer = pointer;
    
    return node;
}

AddrOfNode* ast_addr_of_new(ASTNode* operand, Position pos) {
    AddrOfNode* node = (AddrOfNode*)malloc(sizeof(AddrOfNode));
    if (!node) return NULL;
    
    node->base.type = AST_ADDR_OF;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->operand = operand;
    
    return node;
}

PortIONode* ast_port_io_new(ASTNode* port, ASTNode* value, int is_input, int size, Position pos) {
    PortIONode* node = (PortIONode*)malloc(sizeof(PortIONode));
    if (!node) return NULL;
    
    node->base.type = AST_PORT_IO;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->port = port;
    node->value = value;
    node->is_input = is_input;
    node->size = size;
    
    return node;
}

MMIOAccessNode* ast_mmio_access_new(ASTNode* address, ASTNode* value, int is_volatile, int size, Position pos) {
    MMIOAccessNode* node = (MMIOAccessNode*)malloc(sizeof(MMIOAccessNode));
    if (!node) return NULL;
    
    node->base.type = AST_MMIO_ACCESS;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->address = address;
    node->value = value;
    node->is_volatile = is_volatile;
    node->size = size;
    
    return node;
}

ExternDeclNode* ast_extern_decl_new(const char* name, const char* abi, ASTNode* params, ASTNode* return_type, const char* library, Position pos) {
    ExternDeclNode* node = (ExternDeclNode*)malloc(sizeof(ExternDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_EXTERN_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->abi = str_dup(abi);
    node->library = str_dup(library);
    node->params = params;
    node->return_type = return_type;
    
    return node;
}

AttributeNode* ast_attribute_new(const char* name, ASTNode* args, Position pos) {
    AttributeNode* node = (AttributeNode*)malloc(sizeof(AttributeNode));
    if (!node) return NULL;
    
    node->base.type = AST_ATTRIBUTE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->args = args;
    
    return node;
}

VolatileExprNode* ast_volatile_expr_new(ASTNode* expr, Position pos) {
    VolatileExprNode* node = (VolatileExprNode*)malloc(sizeof(VolatileExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_VOLATILE_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    
    return node;
}

ParallelForNode* ast_parallel_for_new(ASTNode* init, ASTNode* condition, ASTNode* increment, ASTNode* body, const char* schedule_type, int chunk_size, Position pos) {
    ParallelForNode* node = (ParallelForNode*)malloc(sizeof(ParallelForNode));
    if (!node) return NULL;
    
    node->base.type = AST_PARALLEL_FOR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->init = init;
    node->condition = condition;
    node->increment = increment;
    node->body = body;
    node->schedule_type = str_dup(schedule_type);
    node->chunk_size = chunk_size;
    
    return node;
}

ParallelReduceNode* ast_parallel_reduce_new(ASTNode* array, ASTNode* init_value, ASTNode* reduction_func, const char* operation, Position pos) {
    ParallelReduceNode* node = (ParallelReduceNode*)malloc(sizeof(ParallelReduceNode));
    if (!node) return NULL;
    
    node->base.type = AST_PARALLEL_REDUCE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->array = array;
    node->init_value = init_value;
    node->reduction_func = reduction_func;
    node->operation = str_dup(operation);
    
    return node;
}

BarrierCallNode* ast_barrier_call_new(const char* barrier_name, Position pos) {
    BarrierCallNode* node = (BarrierCallNode*)malloc(sizeof(BarrierCallNode));
    if (!node) return NULL;
    
    node->base.type = AST_BARRIER_CALL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->barrier_name = str_dup(barrier_name);
    
    return node;
}

AtomicExprNode* ast_atomic_expr_new(ASTNode* expr, const char* operation, ASTNode* operand, Position pos) {
    AtomicExprNode* node = (AtomicExprNode*)malloc(sizeof(AtomicExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_ATOMIC_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    node->operation = str_dup(operation);
    node->operand = operand;
    
    return node;
}

ThreadLocalDeclNode* ast_thread_local_decl_new(const char* name, ASTNode* type, ASTNode* init_value, Position pos) {
    ThreadLocalDeclNode* node = (ThreadLocalDeclNode*)malloc(sizeof(ThreadLocalDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_THREAD_LOCAL_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->type = type;
    node->init_value = init_value;
    
    return node;
}

MatchExprNode* ast_match_expr_new(ASTNode* expr, ASTNode* cases, Position pos) {
    MatchExprNode* node = (MatchExprNode*)malloc(sizeof(MatchExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_MATCH_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    node->cases = cases;
    
    return node;
}

MatchCaseNode* ast_match_case_new(ASTNode* pattern, ASTNode* guard, ASTNode* body, Position pos) {
    MatchCaseNode* node = (MatchCaseNode*)malloc(sizeof(MatchCaseNode));
    if (!node) return NULL;
    
    node->base.type = AST_MATCH_CASE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->pattern = pattern;
    node->guard = guard;
    node->body = body;
    
    return node;
}

PatternNode* ast_pattern_new(PatternType pattern_type, Position pos) {
    PatternNode* node = (PatternNode*)malloc(sizeof(PatternNode));
    if (!node) return NULL;
    
    node->base.type = AST_PATTERN;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->pattern_type = pattern_type;
    
    // Initialize the union to safe defaults
    memset(&node->data, 0, sizeof(node->data));
    
    return node;
}

GuardConditionNode* ast_guard_condition_new(ASTNode* condition, Position pos) {
    GuardConditionNode* node = (GuardConditionNode*)malloc(sizeof(GuardConditionNode));
    if (!node) return NULL;
    
    node->base.type = AST_GUARD_CONDITION;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->condition = condition;
    
    return node;
}

KernelDeclNode* ast_kernel_decl_new(const char* name, ASTNode* params, ASTNode* return_type, ASTNode* body, GPUTargetArch target_arch, Position pos) {
    KernelDeclNode* node = (KernelDeclNode*)malloc(sizeof(KernelDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_KERNEL_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->params = params;
    node->return_type = return_type;
    node->body = body;
    node->target_arch = target_arch;
    node->max_threads_per_block = 1024; // Default CUDA value
    node->shared_memory_size = 0;
    node->is_inline = 0;
    
    return node;
}

KernelLaunchNode* ast_kernel_launch_new(ASTNode* kernel_func, ASTNode* grid_dim, ASTNode* block_dim, ASTNode* args, Position pos) {
    KernelLaunchNode* node = (KernelLaunchNode*)malloc(sizeof(KernelLaunchNode));
    if (!node) return NULL;
    
    node->base.type = AST_KERNEL_LAUNCH;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->kernel_func = kernel_func;
    node->grid_dim = grid_dim;
    node->block_dim = block_dim;
    node->args = args;
    node->shared_mem_size = NULL;
    node->stream = NULL;
    
    return node;
}

GPUMemoryAllocNode* ast_gpu_memory_alloc_new(ASTNode* size, ASTNode* element_type, GPUMemoryType memory_type, Position pos) {
    GPUMemoryAllocNode* node = (GPUMemoryAllocNode*)malloc(sizeof(GPUMemoryAllocNode));
    if (!node) return NULL;
    
    node->base.type = AST_GPU_MEMORY_ALLOC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->size = size;
    node->element_type = element_type;
    node->memory_type = memory_type;
    node->is_managed = 0;
    node->alignment = 0;
    
    return node;
}

GPUMemoryCopyNode* ast_gpu_memory_copy_new(ASTNode* dest, ASTNode* src, ASTNode* size, int direction, Position pos) {
    GPUMemoryCopyNode* node = (GPUMemoryCopyNode*)malloc(sizeof(GPUMemoryCopyNode));
    if (!node) return NULL;
    
    node->base.type = AST_GPU_MEMORY_COPY;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->dest = dest;
    node->src = src;
    node->size = size;
    node->direction = direction;
    node->is_async = 0;
    node->stream = NULL;
    
    return node;
}

GPUSyncNode* ast_gpu_sync_new(int sync_type, ASTNode* stream, ASTNode* event, Position pos) {
    GPUSyncNode* node = (GPUSyncNode*)malloc(sizeof(GPUSyncNode));
    if (!node) return NULL;
    
    node->base.type = AST_GPU_SYNC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->sync_type = sync_type;
    node->stream = stream;
    node->event = event;
    
    return node;
}

GPUIntrinsicNode* ast_gpu_intrinsic_new(const char* intrinsic_name, ASTNode* args, GPUExecutionContext context, Position pos) {
    GPUIntrinsicNode* node = (GPUIntrinsicNode*)malloc(sizeof(GPUIntrinsicNode));
    if (!node) return NULL;
    
    node->base.type = AST_GPU_INTRINSIC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->intrinsic_name = str_dup(intrinsic_name);
    node->args = args;
    node->context = context;
    
    return node;
}

// WebAssembly constructors

WasmExportNode* ast_wasm_export_new(const char* export_name, ASTNode* item, const char* export_type, Position pos) {
    WasmExportNode* node = (WasmExportNode*)malloc(sizeof(WasmExportNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_EXPORT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->export_name = str_dup(export_name);
    node->item = item;
    node->export_type = str_dup(export_type);
    
    return node;
}

WasmImportNode* ast_wasm_import_new(const char* module_name, const char* import_name, const char* local_name, const char* import_type, ASTNode* signature, Position pos) {
    WasmImportNode* node = (WasmImportNode*)malloc(sizeof(WasmImportNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_IMPORT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->module_name = str_dup(module_name);
    node->import_name = str_dup(import_name);
    node->local_name = str_dup(local_name);
    node->import_type = str_dup(import_type);
    node->signature = signature;
    
    return node;
}

WasmMemoryNode* ast_wasm_memory_new(ASTNode* min_pages, ASTNode* max_pages, int is_shared, Position pos) {
    WasmMemoryNode* node = (WasmMemoryNode*)malloc(sizeof(WasmMemoryNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_MEMORY;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->min_pages = min_pages;
    node->max_pages = max_pages;
    node->is_shared = is_shared;
    node->is_exported = 0;
    node->export_name = NULL;
    
    return node;
}

WasmTableNode* ast_wasm_table_new(WasmValueType element_type, ASTNode* min_size, ASTNode* max_size, Position pos) {
    WasmTableNode* node = (WasmTableNode*)malloc(sizeof(WasmTableNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_TABLE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->element_type = element_type;
    node->min_size = min_size;
    node->max_size = max_size;
    node->is_exported = 0;
    node->export_name = NULL;
    
    return node;
}

WasmGlobalNode* ast_wasm_global_new(const char* name, WasmValueType value_type, int is_mutable, ASTNode* init_value, Position pos) {
    WasmGlobalNode* node = (WasmGlobalNode*)malloc(sizeof(WasmGlobalNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_GLOBAL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->value_type = value_type;
    node->is_mutable = is_mutable;
    node->init_value = init_value;
    node->is_exported = 0;
    node->export_name = NULL;
    
    return node;
}

WasmTypeNode* ast_wasm_type_new(const char* name, ASTNode* params, ASTNode* results, Position pos) {
    WasmTypeNode* node = (WasmTypeNode*)malloc(sizeof(WasmTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->params = params;
    node->results = results;
    
    return node;
}

WasmStartNode* ast_wasm_start_new(ASTNode* function, Position pos) {
    WasmStartNode* node = (WasmStartNode*)malloc(sizeof(WasmStartNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_START;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->function = function;
    
    return node;
}

WasmElemNode* ast_wasm_elem_new(ASTNode* table_index, ASTNode* offset, ASTNode* elements, Position pos) {
    WasmElemNode* node = (WasmElemNode*)malloc(sizeof(WasmElemNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_ELEM;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->table_index = table_index;
    node->offset = offset;
    node->elements = elements;
    
    return node;
}

WasmDataNode* ast_wasm_data_new(ASTNode* memory_index, ASTNode* offset, ASTNode* data, Position pos) {
    WasmDataNode* node = (WasmDataNode*)malloc(sizeof(WasmDataNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_DATA;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->memory_index = memory_index;
    node->offset = offset;
    node->data = data;
    
    return node;
}

JSInteropNode* ast_js_interop_new(JSInteropType interop_type, const char* object_name, const char* property_name, ASTNode* args, WasmEnvironment target_env, Position pos) {
    JSInteropNode* node = (JSInteropNode*)malloc(sizeof(JSInteropNode));
    if (!node) return NULL;
    
    node->base.type = AST_JS_INTEROP;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->interop_type = interop_type;
    node->object_name = str_dup(object_name);
    node->property_name = str_dup(property_name);
    node->args = args;
    node->target_env = target_env;
    
    return node;
}

DOMAccessNode* ast_dom_access_new(const char* api_name, const char* method_name, ASTNode* args, int is_property, Position pos) {
    DOMAccessNode* node = (DOMAccessNode*)malloc(sizeof(DOMAccessNode));
    if (!node) return NULL;
    
    node->base.type = AST_DOM_ACCESS;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->api_name = str_dup(api_name);
    node->method_name = str_dup(method_name);
    node->args = args;
    node->is_property = is_property;
    
    return node;
}

IfStmtNode* ast_if_stmt_new(ASTNode* condition, ASTNode* then_stmt, ASTNode* else_stmt, Position pos) {
    IfStmtNode* node = (IfStmtNode*)malloc(sizeof(IfStmtNode));
    if (!node) return NULL;

    node->base.type = AST_IF_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->condition = condition;
    node->then_stmt = then_stmt;
    node->else_stmt = else_stmt;

    return node;
}

ForStmtNode* ast_for_stmt_new(ASTNode* init, ASTNode* condition, ASTNode* post, ASTNode* body, Position pos) {
    ForStmtNode* node = (ForStmtNode*)malloc(sizeof(ForStmtNode));
    if (!node) return NULL;

    node->base.type = AST_FOR_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->init = init;
    node->condition = condition;
    node->post = post;
    node->body = body;

    return node;
}

// Utility functions

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