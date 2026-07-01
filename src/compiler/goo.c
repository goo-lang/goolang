// Goo Compiler Driver
// This is the main entry point for the Goo compiler

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <sys/stat.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "codegen.h"
// #include "errors/error.h"  // TODO: Update to use new error API
#include "runtime.h"
#include "import_resolver.h"

// Compiler version
#define GOO_VERSION "0.1.0"

// Compiler options
typedef struct CompilerOptions {
    char* input_file;
    char* output_file;
    bool emit_llvm_ir;
    bool emit_ast;
    bool emit_tokens;
    bool optimize;
    int opt_level;
    bool debug_info;
    bool verbose;
    bool run_after_compile;
    bool dump_packages;   // hidden debug flag: print import-graph in topo order
    char** link_libs;
    int link_lib_count;
} CompilerOptions;

// Forward declarations
static void print_usage(const char* program_name);
static void print_version(void);
static CompilerOptions* parse_arguments(int argc, char* argv[]);
static bool compile_file(const char* filename, CompilerOptions* options);
static char* read_file(const char* filename);
static bool write_file(const char* filename, const char* content);
static char* get_output_filename(const char* input_file, const char* output_file);

int main(int argc, char* argv[]) {
    // Parse command line arguments
    CompilerOptions* options = parse_arguments(argc, argv);
    if (!options) {
        return 1;
    }
    
    // Initialize error handling system
    // error_init(); // TODO: Update to use new error API
    
    // Compile the input file
    bool success = compile_file(options->input_file, options);
    
    // Cleanup
    free(options->output_file);
    if (options->link_libs) {
        for (int i = 0; i < options->link_lib_count; i++) {
            free(options->link_libs[i]);
        }
        free(options->link_libs);
    }
    free(options);
    
    // error_cleanup(); // TODO: Update to use new error API
    
    return success ? 0 : 1;
}

static void print_usage(const char* program_name) {
    printf("Usage: %s [options] <input-file>\n", program_name);
    printf("Options:\n");
    printf("  -o, --output <file>      Output file name (default: <input>.out)\n");
    printf("  -O, --optimize <level>   Optimization level (0-3, default: 0)\n");
    printf("  -g, --debug              Generate debug information\n");
    printf("  -v, --verbose            Verbose output\n");
    printf("  -r, --run                Run the program after compilation\n");
    printf("  -l, --link <lib>         Link with library\n");
    printf("  --emit-llvm              Emit LLVM IR instead of executable\n");
    printf("  --emit-ast               Emit AST (for debugging)\n");
    printf("  --emit-tokens            Emit tokens (for debugging)\n");
    printf("  -h, --help               Show this help message\n");
    printf("  --version                Show version information\n");
}

static void print_version(void) {
    printf("Goo Compiler v%s\n", GOO_VERSION);
    printf("Copyright (c) 2024 Goo Contributors\n");
#if LLVM_AVAILABLE
    printf("LLVM backend: enabled\n");
#else
    printf("LLVM backend: disabled (using interpreter mode)\n");
#endif
}

static CompilerOptions* parse_arguments(int argc, char* argv[]) {
    CompilerOptions* options = calloc(1, sizeof(CompilerOptions));
    if (!options) {
        fprintf(stderr, "Error: Out of memory\n");
        return NULL;
    }
    
    // Default values
    options->opt_level = 0;
    
    static struct option long_options[] = {
        {"output", required_argument, 0, 'o'},
        {"optimize", required_argument, 0, 'O'},
        {"debug", no_argument, 0, 'g'},
        {"verbose", no_argument, 0, 'v'},
        {"run", no_argument, 0, 'r'},
        {"link", required_argument, 0, 'l'},
        {"emit-llvm", no_argument, 0, 0},
        {"emit-ast", no_argument, 0, 0},
        {"emit-tokens", no_argument, 0, 0},
        {"dump-packages", no_argument, 0, 0},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 0},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "o:O:gvrl:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 0:
                // Long option
                if (strcmp(long_options[option_index].name, "emit-llvm") == 0) {
                    options->emit_llvm_ir = true;
                } else if (strcmp(long_options[option_index].name, "emit-ast") == 0) {
                    options->emit_ast = true;
                } else if (strcmp(long_options[option_index].name, "emit-tokens") == 0) {
                    options->emit_tokens = true;
                } else if (strcmp(long_options[option_index].name, "dump-packages") == 0) {
                    options->dump_packages = true;
                } else if (strcmp(long_options[option_index].name, "version") == 0) {
                    print_version();
                    free(options);
                    exit(0);
                }
                break;
                
            case 'o':
                options->output_file = strdup(optarg);
                break;
                
            case 'O':
                options->opt_level = atoi(optarg);
                if (options->opt_level < 0 || options->opt_level > 3) {
                    fprintf(stderr, "Error: Invalid optimization level: %s\n", optarg);
                    free(options);
                    return NULL;
                }
                options->optimize = (options->opt_level > 0);
                break;
                
            case 'g':
                options->debug_info = true;
                break;
                
            case 'v':
                options->verbose = true;
                break;
                
            case 'r':
                options->run_after_compile = true;
                break;
                
            case 'l':
                options->link_libs = realloc(options->link_libs, 
                                           (options->link_lib_count + 1) * sizeof(char*));
                options->link_libs[options->link_lib_count++] = strdup(optarg);
                break;
                
            case 'h':
                print_usage(argv[0]);
                free(options);
                exit(0);
                
            default:
                print_usage(argv[0]);
                free(options);
                return NULL;
        }
    }
    
    // Check for input file
    if (optind >= argc) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        free(options);
        return NULL;
    }
    
    options->input_file = argv[optind];
    
    // Generate default output filename if not specified
    if (!options->output_file) {
        options->output_file = get_output_filename(options->input_file, NULL);
    }
    
    return options;
}

static char* read_file(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file: %s\n", filename);
        return NULL;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Allocate buffer
    char* buffer = malloc(size + 1);
    if (!buffer) {
        fclose(file);
        fprintf(stderr, "Error: Out of memory reading file: %s\n", filename);
        return NULL;
    }
    
    // Read file
    size_t read_size = fread(buffer, 1, size, file);
    buffer[read_size] = '\0';
    
    fclose(file);
    return buffer;
}

static bool write_file(const char* filename, const char* content) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        fprintf(stderr, "Error: Cannot create file: %s\n", filename);
        return false;
    }
    
    fprintf(file, "%s", content);
    fclose(file);
    return true;
}

static char* get_output_filename(const char* input_file, const char* output_file) {
    if (output_file) {
        return strdup(output_file);
    }
    
    // Generate default output name
    char* base = strdup(input_file);
    char* dot = strrchr(base, '.');
    // Accept both Goo's own `.goo` and real Go's `.go` so the compiler can be
    // pointed at actual Go source files (Go-compatibility). Strip either so
    // `foo.go`/`foo.goo` default to the `foo.out` executable name.
    if (dot && (strcmp(dot, ".goo") == 0 || strcmp(dot, ".go") == 0)) {
        *dot = '\0';
    }
    
    // Append .out for executable
    char* result = malloc(strlen(base) + 5);
    sprintf(result, "%s.out", base);
    
    free(base);
    return result;
}

// ---------------------------------------------------------------------------
// Import-graph walk (Task 3, stdlib Phase 0)
//
// Before type-checking main, resolve every transitively-imported package,
// parse it, detect import cycles, and produce a topological (leaves-first)
// ordering. No type-checking/codegen of packages is wired here yet (Tasks
// 4/5): the walk is exercised via the hidden `--dump-packages` flag.
//
// The parser is global-state (lexer_bridge.c: parse_input is self-contained
// — its own lexer, state reset on entry, sets global `ast_root`). We snapshot
// `ast_root` immediately after each parse and detach it (ast_root = NULL) so a
// later parse can't clobber a package AST we already own. AST constructors
// str_dup every stored string, so each source buffer is independent of its
// AST; we nonetheless keep every buffer alive until the walk is torn down
// (belt-and-suspenders, per the task brief).
// ---------------------------------------------------------------------------

extern ASTNode* ast_root;

// Per-file static strdup — house idiom (see import_resolver.c, ast/*.c)
// rather than POSIX strdup, to avoid -std=c23 feature-macro friction.
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) memcpy(dup, str, len + 1);
    return dup;
}

// Tri-color state for cycle detection: unvisited -> in-progress -> done.
typedef enum { PKG_UNVISITED = 0, PKG_IN_PROGRESS = 1, PKG_DONE = 2 } PkgState;

typedef struct {
    char* import_path;   // registry key (owned)
    char* name;          // package short name (owned)
    ASTNode* ast;        // parsed ProgramNode snapshot (owned)
    PkgState state;
} PkgEntry;

typedef struct {
    PkgEntry** entries;     // registry: one per unique import path
    size_t entry_count;
    size_t entry_cap;

    PkgEntry** ordered;     // topological finish order (leaves first)
    size_t ordered_count;

    char** sources;         // every source buffer, freed at teardown
    size_t source_count;
    size_t source_cap;
} PkgGraph;

static PkgEntry* pkg_graph_find(PkgGraph* g, const char* import_path) {
    for (size_t i = 0; i < g->entry_count; i++) {
        if (strcmp(g->entries[i]->import_path, import_path) == 0) {
            return g->entries[i];
        }
    }
    return NULL;
}

static PkgEntry* pkg_graph_add(PkgGraph* g, const char* import_path) {
    if (g->entry_count == g->entry_cap) {
        size_t new_cap = g->entry_cap ? g->entry_cap * 2 : 4;
        PkgEntry** grown = realloc(g->entries, new_cap * sizeof(PkgEntry*));
        if (!grown) return NULL;
        g->entries = grown;
        g->entry_cap = new_cap;
    }
    PkgEntry* e = calloc(1, sizeof(PkgEntry));
    if (!e) return NULL;
    e->import_path = str_dup(import_path);
    if (!e->import_path) { free(e); return NULL; }
    e->state = PKG_UNVISITED;
    g->entries[g->entry_count++] = e;
    return e;
}

// Keep a source buffer alive for the lifetime of the walk.
static int pkg_graph_keep_source(PkgGraph* g, char* buf) {
    if (g->source_count == g->source_cap) {
        size_t new_cap = g->source_cap ? g->source_cap * 2 : 4;
        char** grown = realloc(g->sources, new_cap * sizeof(char*));
        if (!grown) return -1;
        g->sources = grown;
        g->source_cap = new_cap;
    }
    g->sources[g->source_count++] = buf;
    return 0;
}

static int pkg_graph_append_ordered(PkgGraph* g, PkgEntry* e) {
    // ordered never exceeds entry_count; entries[] is already grown, so a
    // parallel array of the same capacity is safe to (re)allocate lazily.
    PkgEntry** grown = realloc(g->ordered, (g->ordered_count + 1) * sizeof(PkgEntry*));
    if (!grown) return -1;
    g->ordered = grown;
    g->ordered[g->ordered_count++] = e;
    return 0;
}

static void pkg_graph_free(PkgGraph* g) {
    for (size_t i = 0; i < g->entry_count; i++) {
        PkgEntry* e = g->entries[i];
        if (!e) continue;
        if (e->ast) ast_node_free(e->ast);
        free(e->import_path);
        free(e->name);
        free(e);
    }
    free(g->entries);
    free(g->ordered);
    for (size_t i = 0; i < g->source_count; i++) free(g->sources[i]);
    free(g->sources);
    memset(g, 0, sizeof(*g));
}

// Concatenate a package's *.go files into a single source buffer (Go
// semantics: a package is the union of its files). Returns a malloc'd
// NUL-terminated buffer the caller owns, or NULL on error.
static char* concat_package_sources(const PackageSource* ps) {
    size_t total = 1;  // trailing NUL
    char** parts = calloc(ps->file_count, sizeof(char*));
    if (!parts) return NULL;
    for (size_t i = 0; i < ps->file_count; i++) {
        parts[i] = read_file(ps->files[i]);
        if (!parts[i]) {
            for (size_t j = 0; j < i; j++) free(parts[j]);
            free(parts);
            return NULL;
        }
        total += strlen(parts[i]) + 1;  // +1 for a joining newline
    }
    char* buf = malloc(total);
    if (!buf) {
        for (size_t i = 0; i < ps->file_count; i++) free(parts[i]);
        free(parts);
        return NULL;
    }
    buf[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < ps->file_count; i++) {
        size_t len = strlen(parts[i]);
        memcpy(buf + off, parts[i], len);
        off += len;
        buf[off++] = '\n';
        free(parts[i]);
    }
    buf[off] = '\0';
    free(parts);
    return buf;
}

// Forward decl for mutual recursion.
static int walk_import(PkgGraph* g, const char* import_path);

// The stdlib packages served by the hardcoded C shim (stdlib_package_lookup +
// the codegen goo_* if-chain). These have NO source under GOOROOT, so the
// import-graph walk must SKIP them — otherwise every existing program that does
// `import "fmt"` would fail to resolve. This preserves the sacred backward-compat
// guarantee: a program importing only shim packages walks to an empty graph and
// the per-package pre-pass is a no-op. Keep in sync with the marker list in
// type_checker.c and stdlib_package_lookup in expression_checker.c.
static bool is_stdlib_shim_import(const char* path) {
    static const char* const shim[] = {"fmt", "os", "strings", "math", "strconv", "errors"};
    for (size_t i = 0; i < sizeof(shim) / sizeof(shim[0]); i++) {
        if (strcmp(path, shim[i]) == 0) return true;
    }
    return false;
}

// Walk every import spec in a ProgramNode's import list.
static int walk_program_imports(PkgGraph* g, ASTNode* imports) {
    for (ASTNode* imp = imports; imp; imp = imp->next) {
        if (imp->type != AST_IMPORT_SPEC) continue;
        ImportSpecNode* spec = (ImportSpecNode*)imp;
        if (!spec->path) continue;
        if (is_stdlib_shim_import(spec->path)) continue;  // handled by the shim
        if (walk_import(g, spec->path) != 0) return -1;
    }
    return 0;
}

// Resolve, parse, and topologically place `import_path`. Returns 0 on success,
// -1 on cycle / resolve / parse failure (message already printed to stderr).
static int walk_import(PkgGraph* g, const char* import_path) {
    PkgEntry* existing = pkg_graph_find(g, import_path);
    if (existing) {
        if (existing->state == PKG_DONE) return 0;        // diamond: already placed
        if (existing->state == PKG_IN_PROGRESS) {
            fprintf(stderr,
                    "Error: import cycle detected involving package \"%s\"\n",
                    import_path);
            return -1;
        }
    }

    PkgEntry* e = pkg_graph_add(g, import_path);
    if (!e) { fprintf(stderr, "Error: out of memory resolving imports\n"); return -1; }
    e->state = PKG_IN_PROGRESS;

    PackageSource ps;
    if (resolve_import(import_path, &ps) != 0) {
        fprintf(stderr, "Error: cannot resolve import \"%s\"\n", import_path);
        return -1;
    }
    e->name = str_dup(ps.name);

    char* buf = concat_package_sources(&ps);
    package_source_free(&ps);
    if (!buf) {
        fprintf(stderr, "Error: cannot read sources for package \"%s\"\n", import_path);
        return -1;
    }
    if (pkg_graph_keep_source(g, buf) != 0) {
        free(buf);
        fprintf(stderr, "Error: out of memory resolving imports\n");
        return -1;
    }

    ast_root = NULL;
    if (parse_input(buf, import_path) != 0 || !ast_root) {
        fprintf(stderr, "Error: failed to parse package \"%s\"\n", import_path);
        return -1;
    }
    e->ast = ast_root;   // snapshot immediately
    ast_root = NULL;     // detach so a later parse can't clobber it

    if (e->ast->type == AST_PROGRAM) {
        ProgramNode* prog = (ProgramNode*)e->ast;
        if (walk_program_imports(g, prog->imports) != 0) return -1;
    }

    e->state = PKG_DONE;
    if (pkg_graph_append_ordered(g, e) != 0) {
        fprintf(stderr, "Error: out of memory resolving imports\n");
        return -1;
    }
    return 0;
}

// Drive the import-graph walk from main's import list and, for --dump-packages,
// print the resolved packages in topological order (leaves first) followed by
// "main". Returns true on success.
static bool dump_package_graph(ProgramNode* main_prog) {
    PkgGraph g;
    memset(&g, 0, sizeof(g));

    bool ok = (walk_program_imports(&g, main_prog->imports) == 0);
    if (ok) {
        for (size_t i = 0; i < g.ordered_count; i++) {
            printf("%s\n", g.ordered[i]->import_path);
        }
        printf("main\n");
    }

    pkg_graph_free(&g);
    return ok;
}

// stdlib Phase 0 (Task 4): type-check and codegen every resolved package (in
// topological, leaves-first order) INTO THE SHARED module. Each package's
// top-level (non-method) functions land under a mangled symbol
// goo_pkg__<pkg>__<name> (see function_codegen.c) and its A-Z top-level symbols
// are published into pkg->exports. A TYPE_PACKAGE marker carrying the Package*
// is registered in the current (global) scope so cross-package selector
// resolution (Task 5) can find it. Returns true on success. With zero resolved
// packages the caller never invokes this, keeping the no-import path identical.
static bool compile_resolved_packages(PkgGraph* g, TypeChecker* checker,
                                      CodeGenerator* codegen) {
    for (size_t i = 0; i < g->ordered_count; i++) {
        PkgEntry* e = g->ordered[i];
        Package* p = type_checker_add_package(checker, e->import_path, e->name);
        if (!p) {
            fprintf(stderr, "Error: out of memory registering package \"%s\"\n",
                    e->import_path);
            return false;
        }

        // Register the package identifier as a TYPE_PACKAGE marker in the
        // current (global) scope, carrying the Package* so Task 5's selector
        // resolution can reach p->exports. This marker is conditional on a REAL
        // import (only resolved packages reach here), unlike the always-on
        // hardcoded stdlib markers seeded in type_checker.c.
        Type* pkg_type = type_new(TYPE_PACKAGE);
        Variable* marker = variable_new(e->name, pkg_type, (Position){0, 0, 0, "import"});
        if (marker) {
            marker->is_builtin = 1;
            marker->is_initialized = 1;
            marker->package = p;
            if (!scope_add_variable(checker->current_scope, marker)) {
                variable_free(marker);  // duplicate import of same name — harmless
            }
        }

        // type_check_package leaves the package scope pushed and current_package
        // set (its LIFETIME CONTRACT) so codegen can recover each function's
        // signature and emit it under the mangled symbol; we tear both down
        // right after codegen so main compiles in the global scope with bare
        // names (current_package == NULL).
        bool ok = type_check_package(checker, p, e->ast)
               && codegen_generate_program(codegen, checker, e->ast);
        scope_pop(checker);
        checker->current_package = NULL;
        if (!ok) {
            fprintf(stderr, "Error: failed to compile package \"%s\"\n",
                    e->import_path);
            return false;
        }
    }
    return true;
}

static bool compile_file(const char* filename, CompilerOptions* options) {
    if (options->verbose) {
        printf("Compiling %s...\n", filename);
    }
    
    // Read source file
    char* source = read_file(filename);
    if (!source) {
        return false;
    }
    
    // Phase 1: Lexical Analysis
    if (options->verbose) {
        printf("Phase 1: Lexical analysis...\n");
    }
    
    Lexer* lexer = lexer_new(source, filename);
    if (!lexer) {
        free(source);
        return false;
    }
    
    // Emit tokens if requested
    if (options->emit_tokens) {
        printf("=== TOKENS ===\n");
        Token* token;
        while ((token = lexer_next_token(lexer)) && token->type != TOKEN_EOF) {
            printf("%-15s %s\n", token_type_string(token->type), 
                   token->literal ? token->literal : "");
            token_free(token);
        }
        if (token) token_free(token);
        
        // Reset lexer
        lexer_free(lexer);
        lexer = lexer_new(source, filename);
    }
    
    // Phase 2: Parsing
    if (options->verbose) {
        printf("Phase 2: Parsing...\n");
    }
    
    // Set up parser with current lexer
    extern Lexer* current_lexer;
    current_lexer = lexer;
    
    // Parse the input
    if (parse_input(source, filename) != 0) {
        fprintf(stderr, "Error: Parse failed\n");
        lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Get the AST root
    extern ASTNode* ast_root;
    ASTNode* ast = ast_root;
    if (!ast) {
        fprintf(stderr, "Error: No AST generated\n");
        lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Emit AST if requested
    if (options->emit_ast) {
        printf("=== AST ===\n");
        ast_print(ast, 0);
        printf("\n");
    }

    // Hidden debug flag: walk the import graph from main and print the
    // resolved packages in topological order (leaves first), then "main".
    // Short-circuits before type-checking/codegen — packages are not yet
    // fed into later phases (Tasks 4/5). `ast` (main) was snapshotted above,
    // so the walk clobbering global ast_root is harmless.
    if (options->dump_packages) {
        bool ok = dump_package_graph((ProgramNode*)ast);
        ast_node_free(ast);
        lexer_free(lexer);
        free(source);
        return ok;
    }

    // Phase 3: Type Checking
    if (options->verbose) {
        printf("Phase 3: Type checking...\n");
    }
    
    TypeChecker* type_checker = type_checker_new();
    if (!type_checker) {
        ast_node_free(ast);
            lexer_free(lexer);
        free(source);
        return false;
    }
    
    if (!type_check_program(type_checker, ast)) {
        type_checker_free(type_checker);
        ast_node_free(ast);
        lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Phase 4: Code Generation
    if (options->verbose) {
        printf("Phase 4: Code generation...\n");
    }
    
#if LLVM_AVAILABLE
    CodeGenerator* codegen = codegen_new(basename(options->output_file));
    if (!codegen) {
        type_checker_free(type_checker);
        ast_node_free(ast);
            lexer_free(lexer);
        free(source);
        return false;
    }

    // stdlib Phase 0 (Task 4): if main imports non-shim packages, resolve+parse
    // them (topological, leaves-first) and compile each INTO THIS module before
    // main, so their exported functions exist under their mangled symbols. With
    // no such imports the graph is empty, compile_resolved_packages' loop never
    // runs, and main's codegen below is byte-identical to the no-import path.
    // (`ast` — main — was snapshotted above, so the walk clobbering global
    // ast_root while parsing sub-packages is harmless.)
    {
        PkgGraph pkg_graph;
        memset(&pkg_graph, 0, sizeof(pkg_graph));
        bool pkgs_ok =
            (walk_program_imports(&pkg_graph, ((ProgramNode*)ast)->imports) == 0)
            && compile_resolved_packages(&pkg_graph, type_checker, codegen);
        pkg_graph_free(&pkg_graph);
        if (!pkgs_ok) {
            codegen_free(codegen);
            type_checker_free(type_checker);
            ast_node_free(ast);
            lexer_free(lexer);
            free(source);
            return false;
        }
    }

    // Generate code
    if (!codegen_generate_program(codegen, type_checker, ast)) {
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast);
        lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Emit LLVM IR if requested
    if (options->emit_llvm_ir) {
        char* ir_filename = malloc(strlen(options->output_file) + 4);
        sprintf(ir_filename, "%s.ll", options->output_file);
        codegen_emit_llvm_ir(codegen, ir_filename);
        if (options->verbose) {
            printf("LLVM IR written to: %s\n", ir_filename);
        }
        free(ir_filename);
    }
    
    // Generate executable
    if (!options->emit_llvm_ir || options->emit_llvm_ir) {
        if (!codegen_emit_executable(codegen, options->output_file)) {
            codegen_free(codegen);
            type_checker_free(type_checker);
            ast_node_free(ast);
            lexer_free(lexer);
            free(source);
            return false;
        }
        
        // Make executable
        chmod(options->output_file, 0755);
        
        if (options->verbose) {
            printf("Executable written to: %s\n", options->output_file);
        }
    }
    
    codegen_free(codegen);
#else
    // Fallback: interpreter mode
    fprintf(stderr, "Warning: LLVM not available, cannot generate native code\n");
    fprintf(stderr, "Consider installing LLVM to enable code generation\n");
    
    // For now, just validate the program
    if (options->verbose) {
        printf("Program validated successfully (interpreter mode)\n");
    }
#endif
    
    // Cleanup
    type_checker_free(type_checker);
    ast_node_free(ast);
    lexer_free(lexer);
    free(source);
    
    // Run program if requested
    if (options->run_after_compile && !options->emit_llvm_ir) {
        if (options->verbose) {
            printf("\nRunning %s...\n", options->output_file);
            printf("================\n");
        }
        
        char command[1024];
        snprintf(command, sizeof(command), "./%s", options->output_file);
        int result = system(command);
        
        if (options->verbose) {
            printf("================\n");
            printf("Exit code: %d\n", WEXITSTATUS(result));
        }
    }
    
    return true;
}