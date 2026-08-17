#ifndef WASM_CODEGEN_H
#define WASM_CODEGEN_H

#include "ast.h"
#include "codegen.h"

// WebAssembly code generation interface

// Initialize WebAssembly code generation
int wasm_codegen_init(WasmEnvironment target_env);

// Generate code for WebAssembly AST nodes
int wasm_codegen_export(const WasmExportNode* export_node);
int wasm_codegen_import(const WasmImportNode* import_node);
int wasm_codegen_memory(const WasmMemoryNode* memory_node);
int wasm_codegen_table(const WasmTableNode* table_node);
int wasm_codegen_global(const WasmGlobalNode* global_node);
int wasm_codegen_js_interop(const JSInteropNode* js_node);
int wasm_codegen_dom_access(const DOMAccessNode* dom_node);

// LLVM configuration for WebAssembly
int wasm_configure_llvm_target(void);
int wasm_add_function_attributes(const char* func_name, int is_export);

// WebAssembly intrinsic functions
int wasm_generate_intrinsic(const char* intrinsic_name, void* args);

// Environment-specific code generation
int wasm_generate_environment_specific(WasmEnvironment env);

// Optimization
int wasm_optimize_code(void);

// Module metadata
int wasm_generate_module_metadata(void);

// Main entry point
int generate_wasm_code(const ASTNode* ast, WasmEnvironment target_env, const char* output_file);

// Cleanup
void wasm_codegen_cleanup(void);

#endif // WASM_CODEGEN_H