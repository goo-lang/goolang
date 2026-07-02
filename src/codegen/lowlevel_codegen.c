#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Low-level lowering: channel send/receive and the unsafe/hardware
// extensions (pointer arithmetic, dereference, address-of, port I/O,
// MMIO). Split from expression_codegen.c (refactor, no behavior change).

ValueInfo* codegen_generate_channel_send(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if LLVM_AVAILABLE
    if (!codegen || !checker || !expr || expr->type != AST_BINARY_EXPR) return NULL;
    
    BinaryExprNode* binary = (BinaryExprNode*)expr;
    
    // Generate channel and value expressions
    ValueInfo* channel_val = codegen_generate_expression(codegen, checker, binary->left);
    if (!channel_val) return NULL;
    
    ValueInfo* value_val = codegen_generate_expression(codegen, checker, binary->right);
    if (!value_val) return NULL;

    // Auto-load lvalue send values (a field selector returns the field's
    // ADDRESS). Loading here (a) snapshots the value at evaluation time —
    // Go semantics; the old path memcpy'd from the field address at
    // rendezvous — and (b) routes the value through the elem-type coercion
    // below, which the address shortcut bypassed: sending an int32 field
    // into a chan int64 memcpy'd 8 bytes from a 4-byte field (adjacent
    // memory bled into the payload).
    if (value_val->is_lvalue && value_val->goo_type) {
        LLVMTypeRef vt = codegen_type_to_llvm(codegen, value_val->goo_type);
        if (vt) {
            value_val->llvm_value = LLVMBuildLoad2(codegen->builder, vt,
                                                   value_val->llvm_value, "send_load");
            value_val->is_lvalue = 0;
        }
    }

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

    // Cast value to void pointer
    LLVMValueRef value_ptr = value_val->llvm_value;
    if (!value_val->is_lvalue) {
        // Determine the alloca type from the channel's declared element type.
        // Using LLVMTypeOf(value_val->llvm_value) is wrong: a literal 42 may
        // arrive as i32 while the channel's element type is i64.
        // goo_chan_send memcpys elem_size bytes from the pointer; if the
        // alloca is narrower, that is a stack-overread (UB).  Use the
        // channel's element LLVM type instead and sign-extend integer values
        // if needed (Goo integers are signed), mirroring what recv does with
        // elem_goo and what composite_codegen does for struct fields.
        Type* chan_goo_s = channel_val->goo_type;
        Type* elem_goo_s = (chan_goo_s && chan_goo_s->kind == TYPE_CHANNEL)
                           ? chan_goo_s->data.channel.element_type : NULL;
        LLVMTypeRef elem_llvm_s = elem_goo_s
            ? codegen_type_to_llvm(codegen, elem_goo_s)
            : LLVMTypeOf(value_val->llvm_value);  // fallback: keep value's own type

        // Coerce the value to match the channel's element width — int<->int
        // (SExt/ZExt/Trunc), int->float, and float<->float (FPExt/FPTrunc) —
        // via the shared width-coercion helper (codegen_coerce_to_type).
        // Signedness signal (matters for the int<->int arm only): the Goo
        // type of the value being sent, falling back to the channel's
        // element type when none is attached (e.g. an untyped literal with
        // matching width). Goo has both signed (int/int8/16/32/64) and
        // unsigned (uint8/16/32/64) integers; using SExt unconditionally was
        // wrong for unsigned values (e.g. uint32 4000000000 would
        // sign-extend to -294967296 instead of 4000000000).
        Type* widen_ty = value_val->goo_type ? value_val->goo_type : elem_goo_s;
        int use_sext = widen_ty ? type_is_signed(widen_ty) : 1;
        LLVMValueRef send_value = codegen_coerce_to_type(codegen, value_val->llvm_value,
                                                          use_sext, elem_llvm_s);

        LLVMValueRef temp_alloca = LLVMBuildAlloca(codegen->builder, elem_llvm_s, "temp_send_value");
        LLVMBuildStore(codegen->builder, send_value, temp_alloca);
        value_ptr = temp_alloca;
    }

    // Cast to void*
    value_ptr = LLVMBuildBitCast(codegen->builder, value_ptr, void_ptr_type, "value_as_void_ptr");

    // Get the goo_chan_send function
    LLVMTypeRef param_types[] = {
        void_ptr_type,  // goo_channel_t*
        void_ptr_type   // void* data
    };
    LLVMTypeRef send_func_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx), param_types, 2, 0);
    
    LLVMValueRef send_func = LLVMGetNamedFunction(codegen->module, "goo_chan_send");
    if (!send_func) {
        // Declare goo_chan_send if not already declared
        send_func = LLVMAddFunction(codegen->module, "goo_chan_send", send_func_type);
    }
    
    // Call goo_chan_send(channel, value_ptr)
    LLVMValueRef args[] = { channel_val->llvm_value, value_ptr };
    LLVMValueRef result = LLVMBuildCall2(codegen->builder, send_func_type, send_func, args, 2, "send_result");
    
    // Create result value info
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = type_checker_get_builtin(checker, TYPE_INT32);  // Returns int (success/failure)
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    return result_info;
#else
    codegen_error(codegen, expr->pos, "Channel send operations require LLVM");
    return NULL;
#endif
}

ValueInfo* codegen_generate_channel_recv(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if LLVM_AVAILABLE
    if (!codegen || !checker || !expr || expr->type != AST_UNARY_EXPR) return NULL;
    
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    
    // Generate channel expression
    ValueInfo* channel_val = codegen_generate_expression(codegen, checker, unary->operand);
    if (!channel_val) return NULL;
    
    LLVMContextRef ctx = codegen->context;
    // Derive the receive element type from the channel's goo_type (Task 2).
    // Falls back to i32 if the channel value has no type annotation.
    Type* chan_goo = channel_val->goo_type;
    Type* elem_goo = (chan_goo && chan_goo->kind == TYPE_CHANNEL)
                     ? chan_goo->data.channel.element_type : NULL;
    LLVMTypeRef element_type = elem_goo
        ? codegen_type_to_llvm(codegen, elem_goo)
        : LLVMInt32TypeInContext(ctx);   // fallback

    // Allocate space for the received value
    LLVMValueRef result_alloca = LLVMBuildAlloca(codegen->builder, element_type, "recv_result");

    // Cast result alloca to void*
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMValueRef result_ptr = LLVMBuildBitCast(codegen->builder, result_alloca, void_ptr_type, "result_as_void_ptr");

    // Get the goo_chan_recv function
    LLVMTypeRef param_types_recv[] = {
        void_ptr_type,  // goo_channel_t*
        void_ptr_type   // void* data
    };
    LLVMTypeRef recv_func_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx), param_types_recv, 2, 0);
    
    LLVMValueRef recv_func = LLVMGetNamedFunction(codegen->module, "goo_chan_recv");
    if (!recv_func) {
        // Declare goo_chan_recv if not already declared
        recv_func = LLVMAddFunction(codegen->module, "goo_chan_recv", recv_func_type);
    }
    
    // Call goo_chan_recv(channel, result_ptr)
    LLVMValueRef args[] = { channel_val->llvm_value, result_ptr };
    LLVMValueRef success __attribute__((unused)) = LLVMBuildCall2(codegen->builder, recv_func_type, recv_func, args, 2, "recv_success");
    
    // For simplicity, assume the receive always succeeds and load the value
    // In a complete implementation, we'd need to check the success flag
    LLVMValueRef received_value = LLVMBuildLoad2(codegen->builder, element_type, result_alloca, "received_value");
    
    // Create result value info
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = received_value;
    result_info->goo_type = elem_goo
        ? elem_goo : type_checker_get_builtin(checker, TYPE_INT32);
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    return result_info;
#else
    codegen_error(codegen, expr->pos, "Channel receive operations require LLVM");
    return NULL;
#endif
}
// Unsafe operation implementations

ValueInfo* codegen_generate_ptr_arithmetic(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_PTR_ARITHMETIC) return NULL;
    
    PtrArithmeticNode* ptr_arith = (PtrArithmeticNode*)expr;
    
    // Generate pointer expression
    ValueInfo* ptr_val = codegen_generate_expression(codegen, checker, ptr_arith->pointer);
    if (!ptr_val) {
        codegen_error(codegen, expr->pos, "Failed to generate pointer expression");
        return NULL;
    }
    
    // Generate offset expression
    ValueInfo* offset_val = codegen_generate_expression(codegen, checker, ptr_arith->offset);
    if (!offset_val) {
        value_info_free(ptr_val);
        codegen_error(codegen, expr->pos, "Failed to generate offset expression");
        return NULL;
    }
    
    // Perform pointer arithmetic using GEP
    LLVMValueRef result = LLVMBuildGEP2(codegen->builder,
                                        LLVMInt8Type(),  // i8 for byte arithmetic
                                        ptr_val->llvm_value,
                                        &offset_val->llvm_value, 1,
                                        "ptr_arith");
    
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = ptr_val->goo_type; // Same type as input pointer
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(ptr_val);
    value_info_free(offset_val);
    
    return result_info;
#endif
}

ValueInfo* codegen_generate_ptr_deref(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_PTR_DEREF) return NULL;
    
    PtrDerefNode* ptr_deref = (PtrDerefNode*)expr;
    
    // Generate pointer expression
    ValueInfo* ptr_val = codegen_generate_expression(codegen, checker, ptr_deref->pointer);
    if (!ptr_val) {
        codegen_error(codegen, expr->pos, "Failed to generate pointer expression for dereference");
        return NULL;
    }
    
    // Load from the pointer
    LLVMValueRef result = LLVMBuildLoad2(codegen->builder,
                                         LLVMInt8Type(), // Load as i8 for now
                                         ptr_val->llvm_value,
                                         "ptr_deref");
    
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = NULL; // TODO: Determine pointee type
    result_info->is_lvalue = 1; // Dereferenced pointer is an lvalue
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(ptr_val);
    
    return result_info;
#endif
}

ValueInfo* codegen_generate_addr_of(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_ADDR_OF) return NULL;
    
    AddrOfNode* addr_of = (AddrOfNode*)expr;
    
    // Generate the operand expression - must be an lvalue
    ValueInfo* operand_val = codegen_generate_expression(codegen, checker, addr_of->operand);
    if (!operand_val) {
        codegen_error(codegen, expr->pos, "Failed to generate operand expression for address-of");
        return NULL;
    }
    
    if (!operand_val->is_lvalue) {
        value_info_free(operand_val);
        codegen_error(codegen, expr->pos, "Cannot take address of non-lvalue expression");
        return NULL;
    }
    
    // The address is the LLVM value itself (since lvalues store addresses)
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = operand_val->llvm_value;
    result_info->goo_type = NULL; // TODO: Create pointer type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(operand_val);
    
    return result_info;
#endif
}

ValueInfo* codegen_generate_port_io(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_PORT_IO) return NULL;
    
    PortIONode* port_io = (PortIONode*)expr;
    
    // Generate port number expression
    ValueInfo* port_val = codegen_generate_expression(codegen, checker, port_io->port);
    if (!port_val) {
        codegen_error(codegen, expr->pos, "Failed to generate port number expression");
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    
    if (port_io->is_input) {
        // Port input - generate inline assembly for IN instruction
        LLVMTypeRef port_type = LLVMInt16Type();
        LLVMTypeRef func_type = LLVMFunctionType(LLVMInt8Type(), &port_type, 1, 0);
        const char* asm_str = "inb $1, $0";
        const char* constraints = "=a,Nd,~{dirflag},~{fpsr},~{flags}";
        
        LLVMValueRef inline_asm = LLVMGetInlineAsm(func_type, (char*)asm_str, strlen(asm_str),
                                                   (char*)constraints, strlen(constraints),
                                                   1, 0, LLVMInlineAsmDialectIntel, 0);
        
        result = LLVMBuildCall2(codegen->builder, func_type, inline_asm, &port_val->llvm_value, 1, "port_in");
    } else {
        // Port output - generate inline assembly for OUT instruction
        ValueInfo* value_val = codegen_generate_expression(codegen, checker, port_io->value);
        if (!value_val) {
            value_info_free(port_val);
            codegen_error(codegen, expr->pos, "Failed to generate value expression for port output");
            return NULL;
        }
        
        LLVMTypeRef func_type = LLVMFunctionType(LLVMVoidType(), NULL, 0, 0);
        const char* asm_str = "outb $1, $0";
        const char* constraints = "Nd,a,~{dirflag},~{fpsr},~{flags}";
        
        LLVMValueRef inline_asm = LLVMGetInlineAsm(func_type, (char*)asm_str, strlen(asm_str),
                                                   (char*)constraints, strlen(constraints),
                                                   1, 1, LLVMInlineAsmDialectIntel, 0);
        
        LLVMValueRef args[] = { port_val->llvm_value, value_val->llvm_value };
        result = LLVMBuildCall2(codegen->builder, func_type, inline_asm, args, 2, "port_out");
        
        value_info_free(value_val);
    }
    
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = NULL; // TODO: Set appropriate type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(port_val);
    
    return result_info;
#endif
}

ValueInfo* codegen_generate_mmio_access(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_MMIO_ACCESS) return NULL;
    
    MMIOAccessNode* mmio = (MMIOAccessNode*)expr;
    
    // Generate address expression
    ValueInfo* addr_val = codegen_generate_expression(codegen, checker, mmio->address);
    if (!addr_val) {
        codegen_error(codegen, expr->pos, "Failed to generate address expression for MMIO");
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    
    if (mmio->value == NULL) {
        // MMIO read
        LLVMTypeRef load_type = (mmio->size == 1) ? LLVMInt8Type() :
                               (mmio->size == 2) ? LLVMInt16Type() :
                               (mmio->size == 4) ? LLVMInt32Type() : LLVMInt64Type();
        
        result = LLVMBuildLoad2(codegen->builder, load_type, addr_val->llvm_value, "mmio_read");
        
        if (mmio->is_volatile) {
            LLVMSetVolatile(result, 1);
        }
    } else {
        // MMIO write
        ValueInfo* value_val = codegen_generate_expression(codegen, checker, mmio->value);
        if (!value_val) {
            value_info_free(addr_val);
            codegen_error(codegen, expr->pos, "Failed to generate value expression for MMIO write");
            return NULL;
        }
        
        result = LLVMBuildStore(codegen->builder, value_val->llvm_value, addr_val->llvm_value);
        
        if (mmio->is_volatile) {
            LLVMSetVolatile(result, 1);
        }
        
        value_info_free(value_val);
    }
    
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = result;
    result_info->goo_type = NULL; // TODO: Set appropriate type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    value_info_free(addr_val);
    
    return result_info;
#endif
}
