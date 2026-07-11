// Goo Compiler Driver
// This is the main entry point for the Goo compiler

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

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

// P5.3: invocation surface. LEGACY is the original `goo [options] <file>`
// flag form and must stay byte-compatible; BUILD/RUN are the Go-parity
// subcommands (`goo build`, `goo run`) dispatched on argv[1] before getopt
// ever sees the arguments.
typedef enum {
    GOO_MODE_LEGACY,
    GOO_MODE_BUILD,
    GOO_MODE_RUN,
} GooMode;

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
    char** run_args;      // P5.3: program args after `--` (borrowed from main's argv)
    int run_arg_count;
    bool delete_output_after_run;  // P5.3: `goo run` temp binary cleanup
} CompilerOptions;

// Forward declarations
static void print_usage(FILE* out, const char* program_name);
static void print_version(void);
static CompilerOptions* parse_arguments(int argc, char* argv[], GooMode mode);
static bool compile_file(const char* filename, CompilerOptions* options);
static char* read_file(const char* filename);
static bool write_file(const char* filename, const char* content);
static char* get_output_filename(const char* input_file, const char* output_file,
                                 const char* ext);
static int run_program(const char* path, char** args, int arg_count, bool verbose);

int main(int argc, char* argv[]) {
    // P5.3: subcommand dispatch BEFORE getopt — GNU getopt permutes argv, so
    // a bare-word argv[1] must be claimed here or `goo run prog.goo -v`
    // would steal -v from the program. Legacy flag-form invocations
    // (argv[1] starts with '-' or is a filename) fall through untouched.
    GooMode mode = GOO_MODE_LEGACY;
    if (argc >= 2) {
        if (strcmp(argv[1], "help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argv[1], "version") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(argv[1], "build") == 0 || strcmp(argv[1], "run") == 0) {
            mode = (argv[1][0] == 'b') ? GOO_MODE_BUILD : GOO_MODE_RUN;
            // Shift the subcommand out so getopt sees a conventional argv.
            argv[1] = argv[0];
            argv++;
            argc--;
        }
    }

    // Parse command line arguments
    CompilerOptions* options = parse_arguments(argc, argv, mode);
    if (!options) {
        return 1;
    }
    
    // Initialize error handling system
    // error_init(); // TODO: Update to use new error API
    
    // Compile the input file
    bool success = compile_file(options->input_file, options);

    // P5.1: -r (and `goo run`) runs the compiled program and goo's exit code
    // becomes the program's (compile errors keep exiting 1, before any run
    // is attempted).
    int exit_code = success ? 0 : 1;
    if (success && options->run_after_compile && !options->emit_llvm_ir) {
        exit_code = run_program(options->output_file, options->run_args,
                                options->run_arg_count, options->verbose);
    }

    // P5.3: `goo run` compiles to a mkstemp temp binary; remove it even on
    // compile failure (mkstemp already created the empty file).
    if (options->delete_output_after_run) {
        unlink(options->output_file);
    }

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

    return exit_code;
}

// P5.4: usage goes to stdout when explicitly requested (help subcommand,
// -h) and to stderr on error paths — stdout carries only requested output.
static void print_usage(FILE* out, const char* program_name) {
    fprintf(out, "Usage: %s [options] <input-file>\n", program_name);
    fprintf(out, "       %s build [options] <input-file>\n", program_name);
    fprintf(out, "       %s run [options] <input-file> [-- <program args...>]\n", program_name);
    fprintf(out, "       %s help | version\n", program_name);
    fprintf(out, "Subcommands:\n");
    fprintf(out, "  build                    Compile; executable named <stem> in the current directory\n");
    fprintf(out, "  run                      Compile to a temporary binary, run it (forwarding args\n");
    fprintf(out, "                           after --), exit with the program's exit code\n");
    fprintf(out, "Options:\n");
    fprintf(out, "  -o, --output <file>      Output file name (default: <input>.out, or <input>.ll with --emit-llvm)\n");
    fprintf(out, "  -O, --optimize <level>   Optimization level (0-3, default: 0)\n");
    fprintf(out, "  -g, --debug              Generate debug information\n");
    fprintf(out, "  -v, --verbose            Verbose output\n");
    fprintf(out, "  -r, --run                Run the program after compilation (exit code = program's)\n");
    fprintf(out, "  -l, --link <lib>         Link with library\n");
    fprintf(out, "  --emit-llvm              Emit LLVM IR instead of executable\n");
    fprintf(out, "  --emit-ast               Emit AST (for debugging)\n");
    fprintf(out, "  --emit-tokens            Emit tokens (for debugging)\n");
    fprintf(out, "  -h, --help               Show this help message\n");
    fprintf(out, "  --version                Show version information\n");
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

// P5.3: `goo run` compiles to a unique temp binary (deleted after the run).
// mkstemp creates the file 0600; the linker truncates it in place and
// compile_file chmods it 0755 before it is executed.
static char* make_temp_output_path(void) {
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) {
        tmpdir = "/tmp";
    }
    const char template_tail[] = "/goo-run-XXXXXX";
    char* path = malloc(strlen(tmpdir) + sizeof(template_tail));
    if (!path) {
        fprintf(stderr, "Error: Out of memory\n");
        return NULL;
    }
    sprintf(path, "%s%s", tmpdir, template_tail);
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot create temporary file %s: %s\n",
                path, strerror(errno));
        free(path);
        return NULL;
    }
    close(fd);
    return path;
}

static CompilerOptions* parse_arguments(int argc, char* argv[], GooMode mode) {
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
                print_usage(stdout, argv[0]);
                free(options);
                exit(0);

            default:
                print_usage(stderr, argv[0]);
                free(options);
                return NULL;
        }
    }
    
    // Check for input file
    if (optind >= argc) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(stderr, argv[0]);
        free(options);
        return NULL;
    }

    options->input_file = argv[optind];

    // P5.3: mode-specific argument handling.
    switch (mode) {
        case GOO_MODE_RUN:
            if (options->emit_llvm_ir) {
                fprintf(stderr, "Error: --emit-llvm cannot be combined with 'goo run'\n");
                free(options->output_file);
                free(options);
                return NULL;
            }
            // Everything after the input file is the program's argv (the
            // `--` that getopt consumed guards program flags). Borrowed
            // pointers into main's argv — no copies to free.
            options->run_args = &argv[optind + 1];
            options->run_arg_count = argc - optind - 1;
            options->run_after_compile = true;
            break;
        case GOO_MODE_BUILD:
            if (optind + 1 < argc) {
                fprintf(stderr, "Error: unexpected argument '%s' after input file\n",
                        argv[optind + 1]);
                free(options->output_file);
                free(options);
                return NULL;
            }
            break;
        case GOO_MODE_LEGACY:
            break;
    }

    // Generate default output filename if not specified. --emit-llvm writes
    // IR only (P5.2), so its default is <stem>.ll, not the executable name.
    // `goo build` is Go parity: bare <stem>, in the cwd (P5.3); `goo run`
    // compiles to a temp binary that main() deletes after the run.
    if (!options->output_file) {
        switch (mode) {
            case GOO_MODE_BUILD: {
                const char* slash = strrchr(options->input_file, '/');
                const char* base_input = slash ? slash + 1 : options->input_file;
                options->output_file = get_output_filename(base_input, NULL, "");
                break;
            }
            case GOO_MODE_RUN:
                options->output_file = make_temp_output_path();
                if (!options->output_file) {
                    free(options);
                    return NULL;
                }
                options->delete_output_after_run = true;
                break;
            case GOO_MODE_LEGACY:
                options->output_file = get_output_filename(options->input_file, NULL,
                                                           options->emit_llvm_ir ? ".ll" : ".out");
                break;
        }
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

static char* get_output_filename(const char* input_file, const char* output_file,
                                 const char* ext) {
    if (output_file) {
        return strdup(output_file);
    }

    // Generate default output name
    char* base = strdup(input_file);
    char* dot = strrchr(base, '.');
    // Accept both Goo's own `.goo` and real Go's `.go` so the compiler can be
    // pointed at actual Go source files (Go-compatibility). Strip either so
    // `foo.go`/`foo.goo` default to `foo<ext>` (".out" executable, ".ll" IR).
    if (dot && (strcmp(dot, ".goo") == 0 || strcmp(dot, ".go") == 0)) {
        *dot = '\0';
    }

    char* result = malloc(strlen(base) + strlen(ext) + 1);
    sprintf(result, "%s%s", base, ext);

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

    // P4.5: the main .goo file's directory, threaded down to every
    // resolve_import() call in this walk (walk_import, below) so "./name"
    // and bare-name-fallback imports can resolve relative to it. NOT owned
    // by PkgGraph — points into a buffer that outlives the walk (a stack
    // buffer in compile_file); NULL when no source-dir context applies.
    const char* source_dir;
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
    // `strings` is NOT here: it now has a source package (goostd/strings/) with
    // vendored functions (HasPrefix/HasSuffix). It is walked as a source package
    // so those resolve via its exports; the codegen/type-check shim path stays a
    // per-symbol FALLBACK for the functions still implemented as shims
    // (Contains/ToUpper/Split/Join) — resolution is exports-first, then shim.
    //
    // P4.4: normalize first so a nested spelling of a (currently hypothetical)
    // shim alias can never slip past this comparison — a no-op today since
    // neither table entry (unicode/utf8, math/bits) names a shim package, but
    // keeps every raw-import-path comparison funneled through the same single
    // choke point (see normalize_import_path's doc comment).
    path = normalize_import_path(path);
    // P4.6: "time" joins sync as a method-aware bespoke shim (Duration/Time
    // synthesized below, no GOOROOT source dir) — same reasoning as sync's
    // own entry.
    static const char* const shim[] = {"fmt", "os", "math", "errors", "sync", "time"};
    for (size_t i = 0; i < sizeof(shim) / sizeof(shim[0]); i++) {
        if (strcmp(path, shim[i]) == 0) return true;
    }
    return false;
}

// P4.7 (packages-B, B3): mint a synthesized TYPE_STRUCT for sync.Mutex /
// sync.WaitGroup — a single opaque-pointer field (zero-value NULL), so
// `var mu sync.Mutex` allocas 8 zeroed bytes and is usable immediately
// (Go's zero-value contract; see src/runtime/sync_shim.c for the lazy-init
// runtime side). owner_package is stamped directly here (this Type is
// never reached via type_check_type_decl, the ordinary stamping site — see
// that function's own owner_package comment) so B2's cross-package method
// resolution (type_checker_lookup_method / type_receiver_owner_package)
// treats it exactly like a real source-package exported struct: zero new
// checker logic needed beyond this seeding.
static Type* sync_make_opaque_struct(TypeChecker* checker, Package* pkg, const char* name) {
    Type* opaque_ptr = type_pointer(type_checker_get_builtin(checker, TYPE_INT8));
    if (!opaque_ptr) return NULL;

    Type* result = type_new(TYPE_STRUCT);
    if (!result) return NULL;
    result->data.struct_type.fields = calloc(1, sizeof(StructField));
    if (!result->data.struct_type.fields) { free(result); return NULL; }
    result->data.struct_type.field_count = 1;
    result->data.struct_type.fields[0].name = str_dup("_state");
    result->data.struct_type.fields[0].type = opaque_ptr;
    result->data.struct_type.fields[0].offset = 0;
    result->data.struct_type.name = str_dup(name);
    result->size = sizeof(void*);
    result->align = sizeof(void*);
    result->owner_package = pkg;
    return result;
}

// Register `name` -> `type` as an exported TYPE in pkg->exports. is_builtin=1
// mirrors type_check_type_decl's own discriminator (type_checker.c), which
// B1's type_from_ast AST_BASIC_TYPE arm requires before resolving an export
// as a type name (guards against a same-named VALUE export shadowing it —
// see that arm's doc comment).
static void sync_export_type(Package* pkg, const char* name, Type* type) {
    Variable* v = variable_new(name, type, (Position){0, 0, 0, "sync"});
    if (!v) return;
    v->is_builtin = 1;
    v->is_initialized = 1;
    if (!scope_add_variable(pkg->exports, v)) variable_free(v);
}

// Register a method Variable "T__method" directly in pkg->exports, under
// B2's cross-package mangled-name convention (type_checker_lookup_method
// falls back to owner->exports keyed on exactly this name once a lookup in
// the current scope chain misses — true for every sync method call, since
// main's scope chain never held these Variables in the first place).
// is_builtin=0: the call-signature-check gate in type_check_call_expr
// demands `!m->is_builtin`, mirroring an ordinary user-defined method's
// Variable (type_check_function_decl never sets is_builtin either).
// Receiver is always a pointer (*Mutex / *WaitGroup) — every sync method
// mutates shared state, matching Go's sync package.
static void sync_export_method(Package* pkg, Type* recv_struct, const char* method_name,
                                Type** param_types, size_t param_count, Type* return_type) {
    char* mangled = type_method_mangled_name(recv_struct->data.struct_type.name, method_name);
    if (!mangled) return;

    Type* recv_ptr = type_pointer(recv_struct);
    size_t total = param_count + 1;
    Type** all_params = malloc(sizeof(Type*) * total);
    if (!all_params) { free(mangled); return; }
    all_params[0] = recv_ptr;
    for (size_t i = 0; i < param_count; i++) all_params[i + 1] = param_types[i];
    Type* func_type = type_function(all_params, total, return_type);
    free(all_params);

    Variable* v = variable_new(mangled, func_type, (Position){0, 0, 0, "sync"});
    free(mangled);
    if (!v) return;
    v->is_initialized = 1;
    if (!scope_add_variable(pkg->exports, v)) variable_free(v);
}

// Populate the sync package's exports: types Mutex/WaitGroup plus their
// method set (Mutex.Lock/Unlock, WaitGroup.Add/Done/Wait — the roadmap's
// P4.7 acceptance surface). Called once, right after sync's Package is
// created in seed_imported_stdlib_markers below.
static void seed_sync_package_exports(TypeChecker* checker, Package* pkg) {
    Type* void_t = type_checker_get_builtin(checker, TYPE_VOID);
    Type* int_t = type_checker_get_builtin(checker, TYPE_INT64);  // Go "int"

    Type* mutex_t = sync_make_opaque_struct(checker, pkg, "Mutex");
    Type* wg_t = sync_make_opaque_struct(checker, pkg, "WaitGroup");
    if (!mutex_t || !wg_t) return;

    sync_export_type(pkg, "Mutex", mutex_t);
    sync_export_type(pkg, "WaitGroup", wg_t);

    sync_export_method(pkg, mutex_t, "Lock", NULL, 0, void_t);
    sync_export_method(pkg, mutex_t, "Unlock", NULL, 0, void_t);

    Type* add_params[] = { int_t };
    sync_export_method(pkg, wg_t, "Add", add_params, 1, void_t);
    sync_export_method(pkg, wg_t, "Done", NULL, 0, void_t);
    sync_export_method(pkg, wg_t, "Wait", NULL, 0, void_t);
}

// P4.6 (packages-C, C1): mint the Time struct — a single int64 field holding
// wall-clock nanoseconds since the Unix epoch. Unlike sync's opaque-pointer
// field (lazy runtime init required — a zero-filled pthread_mutex_t is not
// portably valid), a plain int64 IS its own valid zero value: `var t
// time.Time` zero-values to nanos=0 and is immediately meaningful (the Unix
// epoch), so no lazy-init machinery is needed on the runtime side at all —
// see src/runtime/time_shim.c, which is a stateless wrap of the platform
// clock primitives. owner_package is stamped directly here for the same
// reason sync_make_opaque_struct stamps it: this Type never passes through
// type_check_type_decl, the ordinary stamping site.
static Type* time_make_time_struct(TypeChecker* checker, Package* pkg) {
    Type* nanos_t = type_checker_get_builtin(checker, TYPE_INT64);
    if (!nanos_t) return NULL;

    Type* result = type_new(TYPE_STRUCT);
    if (!result) return NULL;
    result->data.struct_type.fields = calloc(1, sizeof(StructField));
    if (!result->data.struct_type.fields) { free(result); return NULL; }
    result->data.struct_type.field_count = 1;
    result->data.struct_type.fields[0].name = str_dup("_nanos");
    result->data.struct_type.fields[0].type = nanos_t;
    result->data.struct_type.fields[0].offset = 0;
    result->data.struct_type.name = str_dup("Time");
    result->size = sizeof(int64_t);
    result->align = sizeof(int64_t);
    result->owner_package = pkg;
    return result;
}

// P4.6 (packages-C, C1): mint the Duration type — Go's `type Duration
// int64` — via the same named-type clone type_check_type_decl uses for an
// ordinary user `type MyInt int` (type_checker.c: clone the shared int64
// builtin singleton rather than mutate it in place, since every other int64
// value in the program shares that singleton pointer). This gives
// time.Duration a genuinely distinct Type (so diagnostics print "Duration",
// and `time.Duration` resolves as a real type name via the same
// is_builtin=1 export mechanism sync.Mutex uses) while staying int64-KIND
// compatible for every arithmetic/argument check downstream — those compare
// operand KIND and WIDTH only (type_check_arithmetic_op,
// expression_checker.c's numeric argument-compatibility gate), never name
// identity, which is exactly the effect Go's untyped-constant rule gives
// `50 * time.Millisecond`.
static Type* time_make_duration_type(TypeChecker* checker, Package* pkg) {
    Type* builtin = type_checker_get_builtin(checker, TYPE_INT64);
    if (!builtin) return NULL;
    Type* result = type_copy(builtin);
    if (!result) return NULL;
    free(result->name);
    result->name = str_dup("Duration");
    result->owner_package = pkg;
    return result;
}

// Register `name` -> `type` as an exported TYPE in pkg->exports — same
// contract as sync_export_type (is_builtin=1 is what lets B1's
// type_from_ast AST_BASIC_TYPE arm resolve `time.Duration`/`time.Time` as
// type names, not a same-named value export).
static void time_export_type(Package* pkg, const char* name, Type* type) {
    Variable* v = variable_new(name, type, (Position){0, 0, 0, "time"});
    if (!v) return;
    v->is_builtin = 1;
    v->is_initialized = 1;
    if (!scope_add_variable(pkg->exports, v)) variable_free(v);
}

// Register a package-level VALUE member (Nanosecond/Microsecond/Millisecond/
// Second) — is_builtin stays 0, unlike time_export_type above: these names
// resolve as ordinary VALUES (mirrors math.Pi/os.Args), never as type names,
// so they must not satisfy the type_from_ast is_builtin=1 gate that
// time_export_type's Duration/Time exports rely on.
static void time_export_value(Package* pkg, const char* name, Type* type) {
    Variable* v = variable_new(name, type, (Position){0, 0, 0, "time"});
    if (!v) return;
    v->is_initialized = 1;
    if (!scope_add_variable(pkg->exports, v)) variable_free(v);
}

// Register a plain package-level function Variable (Sleep, Now — no
// receiver splicing, unlike a method). expression_checker.c's package-
// function-call arm resolves these straight out of pkg->exports by
// identity, same mechanism as a real source-compiled package's exports.
static void time_export_func(Package* pkg, const char* name, Type** param_types,
                              size_t param_count, Type* return_type) {
    Type* func_type = type_function(param_types, param_count, return_type);
    if (!func_type) return;
    Variable* v = variable_new(name, func_type, (Position){0, 0, 0, "time"});
    if (!v) return;
    v->is_initialized = 1;
    if (!scope_add_variable(pkg->exports, v)) variable_free(v);
}

// Register a method Variable "T__method" under B2's cross-package mangled-
// name convention, exactly like sync_export_method, EXCEPT the receiver is
// a VALUE (`recv_struct` itself), not `type_pointer(recv_struct)` — Go: all
// of time.Time's methods (UnixNano included) are value receivers, since
// Time is an immutable value type, unlike every sync primitive (which
// mutates shared state through a pointer receiver).
static void time_export_method(Package* pkg, Type* recv_struct, const char* method_name,
                                Type** param_types, size_t param_count, Type* return_type) {
    char* mangled = type_method_mangled_name(recv_struct->data.struct_type.name, method_name);
    if (!mangled) return;

    size_t total = param_count + 1;
    Type** all_params = malloc(sizeof(Type*) * total);
    if (!all_params) { free(mangled); return; }
    all_params[0] = recv_struct;
    for (size_t i = 0; i < param_count; i++) all_params[i + 1] = param_types[i];
    Type* func_type = type_function(all_params, total, return_type);
    free(all_params);

    Variable* v = variable_new(mangled, func_type, (Position){0, 0, 0, "time"});
    free(mangled);
    if (!v) return;
    v->is_initialized = 1;
    if (!scope_add_variable(pkg->exports, v)) variable_free(v);
}

// Populate the time package's exports: types Duration/Time, functions
// Sleep(Duration)/Now() Time, method Time.UnixNano() int64, and the four
// Duration constants. Mirrors seed_sync_package_exports's structure but
// every export here is a plain package-level symbol (no receiver splicing)
// except UnixNano, and UnixNano's receiver is a VALUE (Go: `func (t Time)
// UnixNano() int64`) rather than sync's uniform pointer receiver — see
// time_export_method's doc comment. Called once, right after time's Package
// is created in seed_imported_stdlib_markers below.
static void seed_time_package_exports(TypeChecker* checker, Package* pkg) {
    Type* void_t = type_checker_get_builtin(checker, TYPE_VOID);
    Type* int64_t_ty = type_checker_get_builtin(checker, TYPE_INT64);

    Type* duration_t = time_make_duration_type(checker, pkg);
    Type* time_t_ty = time_make_time_struct(checker, pkg);
    if (!duration_t || !time_t_ty) return;

    time_export_type(pkg, "Duration", duration_t);
    time_export_type(pkg, "Time", time_t_ty);

    Type* sleep_params[] = { duration_t };
    time_export_func(pkg, "Sleep", sleep_params, 1, void_t);
    time_export_func(pkg, "Now", NULL, 0, time_t_ty);

    time_export_method(pkg, time_t_ty, "UnixNano", NULL, 0, int64_t_ty);

    // Duration constants (Go: Nanosecond=1, Microsecond=1e3, Millisecond=1e6,
    // Second=1e9). Only the TYPE is seeded here — expression_checker.c's
    // generic package-export lookup resolves `time.Millisecond` to this
    // Duration Type with no further checker changes needed (same path Sleep/
    // Now use). The actual constant VALUE is emitted by a codegen intercept
    // (composite_codegen.c, math.Pi's pattern) since there is no general
    // codegen path for an arbitrary package-level exported value member.
    time_export_value(pkg, "Nanosecond", duration_t);
    time_export_value(pkg, "Microsecond", duration_t);
    time_export_value(pkg, "Millisecond", duration_t);
    time_export_value(pkg, "Second", duration_t);
}

// stdlib Phase 0 (Task 4): seed a TYPE_PACKAGE marker for each stdlib-shim
// package that main ACTUALLY imports. This replaces the former always-on
// seeding in type_checker.c — markers are now CONDITIONAL on a real `import`
// (Go semantics) and carry a Package* (created via type_checker_add_package),
// unifying stdlib and user-package marker handling. Must run BEFORE main is
// type-checked so `fmt.Println` etc. resolve. Selector resolution for shim
// packages still flows through stdlib_package_lookup (by name), so the empty
// exports scope on the seeded Package is harmless for fmt/os/math/errors.
// P4.7: sync is the one shim package whose exports scope is NOT left empty
// — see seed_sync_package_exports above — because sync exports TYPES with
// METHODS, which stdlib_package_lookup's per-symbol table cannot model (it
// only ever returns a bare Type for a (package, name) pair, never a
// method set). Returns false on OOM.
static bool seed_imported_stdlib_markers(TypeChecker* checker, ASTNode* imports) {
    for (ASTNode* imp = imports; imp; imp = imp->next) {
        if (imp->type != AST_IMPORT_SPEC) continue;
        ImportSpecNode* spec = (ImportSpecNode*)imp;
        if (!spec->path || !is_stdlib_shim_import(spec->path)) continue;
        // Shim import paths are single-word (fmt, os, ...) so path == name;
        // honour an explicit alias if the program wrote one.
        const char* short_name = spec->alias ? spec->alias : spec->path;
        Package* p = type_checker_add_package(checker, spec->path, short_name);
        if (!p) return false;
        if (strcmp(normalize_import_path(spec->path), "sync") == 0) {
            seed_sync_package_exports(checker, p);
        } else if (strcmp(normalize_import_path(spec->path), "time") == 0) {
            seed_time_package_exports(checker, p);
        }
        type_checker_seed_package_marker(checker, short_name, p);
    }
    return true;
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

    // P4.5 review fix: a ".." segment is an explicit rejection, not a
    // resolve — pre-fix, "../pkg" fell through the bare-name tiers and
    // opendir'd <gooroot>/../pkg and <source_dir>/../pkg, escaping both
    // roots (verified by the review). Go rejects ".." in import paths
    // outright; so do we. Checked segment-wise so a package legitimately
    // named "a..b" (weird but not traversal) is unaffected.
    {
        const char* seg = import_path;
        while (seg && *seg) {
            const char* slash = strchr(seg, '/');
            size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
            if (len == 2 && seg[0] == '.' && seg[1] == '.') {
                fprintf(stderr,
                        "Error: invalid import path \"%s\" (\"..\" segments are not allowed)\n",
                        import_path);
                return -1;
            }
            seg = slash ? slash + 1 : NULL;
        }
    }

    PackageSource ps;
    if (resolve_import(import_path, g->source_dir, &ps) != 0) {
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
// "main". Returns true on success. `source_dir` (P4.5) is the main .goo
// file's directory, threaded down so "./name" and bare-name-fallback imports
// resolve the same way here as they do in the real compile path below.
static bool dump_package_graph(ProgramNode* main_prog, const char* source_dir) {
    PkgGraph g;
    memset(&g, 0, sizeof(g));
    g.source_dir = source_dir;

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
        // resolution can reach p->exports. Conditional on a REAL import (only
        // resolved packages reach here). Uses the same single seeding path as
        // the stdlib-shim markers (see seed_imported_stdlib_markers).
        type_checker_seed_package_marker(checker, e->name, p);

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
        // P6 M1 (comptime-wall lift): transfer this package's AST ownership from
        // the graph to its Package. pkg_graph_free (goo.c) runs right after this
        // function, BEFORE main is type-checked/codegen'd — but a comptime-param
        // package function's template FuncDecl (reachable via an export copy's
        // func_decl_node) must survive until main's monomorphizer emits its
        // instances. The Package outlives codegen (freed at type_checker_free),
        // so parking the AST there keeps that template alive; nulling e->ast
        // makes pkg_graph_free skip it (no double free). Non-comptime imports
        // are unaffected — they simply keep their (now Package-owned) AST alive
        // a little longer, freed once at the same final teardown.
        p->owned_ast = e->ast;
        e->ast = NULL;
    }
    return true;
}

// P4.5: extract the directory component of the main .goo file's path, into a
// caller-owned fixed buffer (no heap allocation, so compile_file's many
// early-return paths need no matching free()). "foo.goo" (no slash) yields
// "." (the compiler's invocation directory); "dir/sub/foo.goo" yields
// "dir/sub"; a path starting with '/' preserves a leading "/" root.
static void compute_source_dir(const char* filename, char* out_buf, size_t out_size) {
    if (!filename || out_size == 0) return;
    const char* slash = strrchr(filename, '/');
    if (!slash) { snprintf(out_buf, out_size, "."); return; }
    size_t len = (size_t)(slash - filename);
    if (len == 0) { snprintf(out_buf, out_size, "/"); return; }
    if (len >= out_size) len = out_size - 1;
    memcpy(out_buf, filename, len);
    out_buf[len] = '\0';
}

static bool compile_file(const char* filename, CompilerOptions* options) {
    if (options->verbose) {
        printf("Compiling %s...\n", filename);
    }

    // P4.5: computed once, used by both the --dump-packages walk and the
    // real package-compilation walk below.
    char source_dir[4096];
    compute_source_dir(filename, source_dir, sizeof(source_dir));

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
        bool ok = dump_package_graph((ProgramNode*)ast, source_dir);
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
    
    // stdlib Phase 0 (Task 4): seed TYPE_PACKAGE markers for the stdlib-shim
    // packages main actually imports, BEFORE type-checking main (so its
    // `fmt.Println` etc. resolve). Conditional on real imports — a no-import
    // program seeds nothing and type-checks exactly as before.
    if (!seed_imported_stdlib_markers(type_checker, ((ProgramNode*)ast)->imports)) {
        type_checker_free(type_checker);
        ast_node_free(ast);
        lexer_free(lexer);
        free(source);
        return false;
    }

#if LLVM_AVAILABLE
    // stdlib Phase 0 (Task 5): create the output module and compile the packages
    // main imports BEFORE main is type-checked, so main can resolve cross-package
    // selectors (e.g. mypkg.Double) against the real exported signatures that
    // package compilation publishes into pkg->exports.
    CodeGenerator* codegen = codegen_new(basename(options->output_file));
    if (!codegen) {
        type_checker_free(type_checker);
        ast_node_free(ast);
            lexer_free(lexer);
        free(source);
        return false;
    }

    // P3.10: opt_level travels on the codegen object itself (needed by both
    // codegen_optimize below and the target-machine mapping inside
    // codegen_emit_executable) rather than being threaded through call sites.
    codegen->opt_level = options->opt_level;

    // P3.11: link_libs/link_lib_count travel on the codegen object so
    // codegen_emit_executable's fork/execvp argv construction can append
    // them without changing that function's signature.
    codegen->link_libs = (const char**)options->link_libs;
    codegen->link_lib_count = (size_t)options->link_lib_count;

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
        pkg_graph.source_dir = source_dir;
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
#endif

    // Phase 3: Type Checking (main). Runs AFTER package compilation so that a
    // cross-package selector like `mypkg.Double(21)` resolves against the real
    // exported signature published into pkg->exports above. With no non-shim
    // imports the packages block is a no-op and this stays byte-identical.
    if (!type_check_program(type_checker, ast)) {
#if LLVM_AVAILABLE
        codegen_free(codegen);
#endif
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
    // Generate code for main
    if (!codegen_generate_program(codegen, type_checker, ast)) {
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast);
        lexer_free(lexer);
        free(source);
        return false;
    }

    // P3.10: run the new-PM optimization pipeline (no-op at opt_level 0 —
    // byte-identical contract) BEFORE either emit path, so --emit-llvm shows
    // the optimized IR too — that's how the differential (-O0 vs -O2 IR)
    // gate observes optimization actually happening.
    if (!codegen_optimize(codegen, options->opt_level)) {
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast);
        lexer_free(lexer);
        free(source);
        return false;
    }

    // P5.2: --emit-llvm writes the textual IR to the output path itself and
    // produces NO executable (pre-fix, an always-true conditional wrote the
    // ELF to the -o path and the IR to <path>.ll).
    if (options->emit_llvm_ir) {
        if (!codegen_emit_llvm_ir(codegen, options->output_file)) {
            codegen_free(codegen);
            type_checker_free(type_checker);
            ast_node_free(ast);
            lexer_free(lexer);
            free(source);
            return false;
        }
        if (options->verbose) {
            printf("LLVM IR written to: %s\n", options->output_file);
        }
    } else {
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

    return true;
}

// P5.1: run the compiled program via fork/execv — no shell, so paths with
// spaces/metacharacters are safe, and execv resolves the path against the
// cwd directly (absolute and relative both work, no "./" prefix needed).
// Returns the child's exit code; 128+signal if it was killed (the shell
// convention, so `goo -r` composes with scripts); 127 if it could not be
// executed at all (reported on stderr — never a silent success).
static int run_program(const char* path, char** args, int arg_count, bool verbose) {
    if (verbose) {
        printf("\nRunning %s...\n", path);
        printf("================\n");
    }

    // argv[0] is the binary path (Go's `go run` does the same with its temp
    // binary); the forwarded args follow.
    char** child_argv = malloc(((size_t)arg_count + 2) * sizeof(char*));
    if (!child_argv) {
        fprintf(stderr, "Error: Out of memory\n");
        return 127;
    }
    child_argv[0] = (char*)path;
    for (int i = 0; i < arg_count; i++) {
        child_argv[i + 1] = args[i];
    }
    child_argv[arg_count + 1] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: cannot fork to run %s: %s\n", path, strerror(errno));
        free(child_argv);
        return 127;
    }
    if (pid == 0) {
        execv(path, child_argv);
        fprintf(stderr, "Error: cannot execute %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    free(child_argv);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "Error: waitpid failed for %s: %s\n", path, strerror(errno));
        return 127;
    }

    int exit_code;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    } else {
        exit_code = 127;
    }

    if (verbose) {
        printf("================\n");
        printf("Exit code: %d\n", exit_code);
    }
    return exit_code;
}