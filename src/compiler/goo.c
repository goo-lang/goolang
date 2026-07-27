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
#include <limits.h>   // PATH_MAX, for realpath on a directory entry package
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
#include "test_discovery.h"

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
    GOO_MODE_TEST,
} GooMode;

// Compiler options
typedef struct CompilerOptions {
    // Which subcommand is running. compile_file branches on this for the
    // test-only paths (test-file inclusion, the no-tests exit, _testmain
    // synthesis); it used to be a parse_arguments parameter only.
    GooMode mode;
    // The argument as written: a source file, or a DIRECTORY (`goo build .`,
    // `goo build ./cmd/tool`). Borrowed from argv.
    char* input_file;
    // The entry package's source files. For a file argument this is that one
    // file, so the single-file form runs through exactly the same machinery with
    // n == 1 rather than down a second code path — 473 goldens and the 13-row
    // cli_test all depend on the file form, and a parallel path is how the two
    // would drift. Owned when input_is_dir, else it points at &input_file.
    char** input_files;
    size_t input_file_count;
    bool  input_is_dir;
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
    // Hidden debug flag: print the synthesized _testmain.goo and exit 0. The
    // generated source is never written to disk, so without this a failure
    // inside it is a diagnostic pointing at a file nobody can open.
    bool emit_testmain;
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
        if (strcmp(argv[1], "build") == 0 || strcmp(argv[1], "run") == 0 ||
            strcmp(argv[1], "test") == 0) {
            mode = (argv[1][0] == 'b') ? GOO_MODE_BUILD
                 : (argv[1][0] == 'r') ? GOO_MODE_RUN
                                        : GOO_MODE_TEST;
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
    CompilerOptions* options = xcalloc(1, sizeof(CompilerOptions));
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
        {"emit-testmain", no_argument, 0, 0},
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
                } else if (strcmp(long_options[option_index].name, "emit-testmain") == 0) {
                    options->emit_testmain = true;
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
    
    options->mode = mode;

    // Check for input file. `goo test` with no argument means the current
    // directory, matching `go test`; every other mode still requires one.
    if (optind >= argc) {
        if (mode != GOO_MODE_TEST) {
            fprintf(stderr, "Error: No input file specified\n");
            print_usage(stderr, argv[0]);
            free(options);
            return NULL;
        }
        options->input_file = ".";
    } else {
        options->input_file = argv[optind];
    }

    // A DIRECTORY argument makes the entry package multi-file (`goo build .`,
    // `goo build ./cmd/tool`) — Go's unit of compilation. Expanded through the
    // very same resolve_package_dir that `import "./p"` uses, so the entry
    // package and an imported package can never disagree about which files
    // belong to a package (see is_buildable_source, import_resolver.c).
    {
        struct stat st;
        if (stat(options->input_file, &st) == 0 && S_ISDIR(st.st_mode)) {
            PackageSource ps;
            if (resolve_package_dir_path(options->input_file,
                                         mode == GOO_MODE_TEST, &ps) != 0) {
                fprintf(stderr, "Error: no buildable source files in directory '%s'\n",
                        options->input_file);
                free(options->output_file);
                free(options);
                return NULL;
            }
            options->input_files = ps.files;      // ownership moves to options
            options->input_file_count = ps.file_count;
            options->input_is_dir = true;
            free(ps.name);
            free(ps.import_path);
        } else {
            // File form: n == 1 through the identical path below.
            options->input_files = &options->input_file;
            options->input_file_count = 1;
            options->input_is_dir = false;
        }
    }

    // P5.3: mode-specific argument handling.
    switch (mode) {
        case GOO_MODE_TEST:
            if (options->emit_llvm_ir) {
                fprintf(stderr, "Error: --emit-llvm cannot be combined with 'goo test'\n");
                free(options->output_file);
                free(options);
                return NULL;
            }
            // The test binary takes no arguments of its own in this cut (no
            // -run, no -v), so run_args stays empty. Deliberately NOT
            // `&argv[optind + 1]` like the run case: `goo test` with no
            // argument leaves optind == argc, and that expression would then
            // index one past the end of argv.
            options->run_args = NULL;
            options->run_arg_count = 0;
            options->run_after_compile = true;
            break;
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
                // Go's rule for `go build .`: a DIRECTORY build is named after
                // the directory, not after any file inside it. ".", "./" and a
                // trailing slash all have to become the real directory name,
                // which realpath resolves (resolve_package_dir_path,
                // import_resolver.c) — the resolved package's short name is
                // exactly that, so reuse it instead of re-deriving it here.
                const char* stem;
                char dir_stem[PATH_MAX];
                if (options->input_is_dir) {
                    char resolved[PATH_MAX];
                    const char* src = realpath(options->input_file, resolved)
                                          ? resolved : options->input_file;
                    const char* s = strrchr(src, '/');
                    snprintf(dir_stem, sizeof(dir_stem), "%s",
                             (s && s[1]) ? s + 1 : src);
                    stem = dir_stem;
                } else {
                    const char* slash = strrchr(options->input_file, '/');
                    stem = slash ? slash + 1 : options->input_file;
                }
                options->output_file = get_output_filename(stem, NULL, "");
                // `goo build ./myapp` from the parent directory would name the
                // output "myapp" in the cwd — which is the package directory
                // itself. Go hits the identical collision and refuses with a
                // clear message; without this the linker leaks "cannot open
                // output file myapp: Is a directory" instead. Go's wording:
                //   go: build output "myapp" already exists and is a directory
                struct stat ost;
                if (options->output_file &&
                    stat(options->output_file, &ost) == 0 && S_ISDIR(ost.st_mode)) {
                    fprintf(stderr,
                            "Error: build output \"%s\" already exists and is a directory\n",
                            options->output_file);
                    free(options->output_file);
                    if (options->input_is_dir) {
                        for (size_t i = 0; i < options->input_file_count; i++)
                            free(options->input_files[i]);
                        free(options->input_files);
                    }
                    free(options);
                    return NULL;
                }
                break;
            }
            case GOO_MODE_TEST:
                // Same shape as `goo run`: compile to a temp binary, execute
                // it, propagate its exit status, then remove it. The test
                // binary is never an artifact the user asked for.
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
    // One parsed ProgramNode per FILE of the package (owned). A package is the
    // union of its files, and each file is parsed separately under its own real
    // filename — the filename reaches every diagnostic raised while checking
    // that file, so it must be the true path, not the import path.
    //
    // An ARRAY, deliberately not a ->next chain: ast_node_free recurses into
    // ->next (ast.c), so chaining these roots would make the per-element
    // teardown in pkg_graph_free a double free.
    ASTNode** asts;
    size_t ast_count;
    // The source path each of those ASTs was parsed under (owned, same length
    // as asts). Every Position inside asts[i] stores file_names[i] BY POINTER,
    // so these must live exactly as long as the ASTs — they are transferred to
    // the Package together with them (see Package.owned_file_names, types.h).
    char** file_names;
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
    PkgEntry* e = xcalloc(1, sizeof(PkgEntry));
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
        // Per-file ASTs, each an independent root — see PkgEntry.asts. Slots
        // still owned by the graph are freed here; slots whose ownership moved
        // to the Package (compile_resolved_packages) were nulled there.
        for (size_t a = 0; a < e->ast_count; a++) {
            if (e->asts[a]) ast_node_free(e->asts[a]);
            if (e->file_names) free(e->file_names[a]);
        }
        free(e->asts);
        free(e->file_names);
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

// concat_package_sources lived here. It joined a package's files into one
// buffer so a single parse could cover them all. It was never able to work:
// parser.y's `program` rule admits exactly one package_clause, so any package
// of more than one file failed on the second file's clause. It also destroyed
// diagnostic positions, which is why it is deleted rather than repaired —
// walk_import now parses each file under its own real filename.

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
    static const char* const shim[] = {"fmt", "os", "math", "errors", "sync", "time", "far", "testing"};
    for (size_t i = 0; i < sizeof(shim) / sizeof(shim[0]); i++) {
        if (strcmp(path, shim[i]) == 0) return true;
    }
    return false;
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
// — seed_sync_package_exports/seed_time_package_exports (src/types/
// type_checker.c, shared with that file's own-import seeding path) populate
// it — because sync exports TYPES with METHODS, which stdlib_package_lookup's
// per-symbol table cannot model (it only ever returns a bare Type for a
// (package, name) pair, never a method set). Returns false on OOM.
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
        } else if (strcmp(normalize_import_path(spec->path), "testing") == 0) {
            // Same bespoke-shim reason as sync/time: testing exports a TYPE
            // with a method set, which stdlib_package_lookup's per-symbol
            // table cannot model.
            seed_testing_package_exports(checker, p);
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

    // Parse each file of the package SEPARATELY, under its own real filename.
    //
    // This used to concatenate every file into one buffer and parse that once.
    // Two things were wrong with it. parser.y's `program` rule admits exactly
    // ONE package_clause, so any package of more than one file died on the
    // second file's clause — no multi-file package has ever compiled. And the
    // position in every diagnostic became concat-relative and attributed to the
    // import path, so an error in the second file pointed at a line number that
    // existed in no file and a "filename" that was not a file. PRs #221-#223
    // made diagnostics report true line:column; concatenation would have
    // silently undone that for every package.
    e->asts = calloc(ps.file_count, sizeof(ASTNode*));
    e->file_names = calloc(ps.file_count, sizeof(char*));
    if (!e->asts || !e->file_names) {
        package_source_free(&ps);
        fprintf(stderr, "Error: out of memory resolving imports\n");
        return -1;
    }

    for (size_t fi = 0; fi < ps.file_count; fi++) {
        // Own the path: parse_input stamps THIS pointer into every Position it
        // creates, and `ps` is freed as soon as this loop ends. Passing
        // ps.files[fi] straight through left every diagnostic in the package
        // rendering a filename from freed memory.
        e->file_names[fi] = str_dup(ps.files[fi]);
        if (!e->file_names[fi]) {
            package_source_free(&ps);
            fprintf(stderr, "Error: out of memory resolving imports\n");
            return -1;
        }

        char* buf = read_file(ps.files[fi]);
        if (!buf) {
            fprintf(stderr, "Error: cannot read source file \"%s\" of package \"%s\"\n",
                    ps.files[fi], import_path);
            package_source_free(&ps);
            return -1;
        }
        if (pkg_graph_keep_source(g, buf) != 0) {
            free(buf);
            package_source_free(&ps);
            fprintf(stderr, "Error: out of memory resolving imports\n");
            return -1;
        }

        // ast_root is a bison global: snapshot it into this file's slot and
        // null it immediately, so the NEXT file's parse cannot clobber a
        // pointer we already own (the same snapshot-and-detach discipline the
        // single-file path used).
        ast_root = NULL;
        if (parse_input(buf, e->file_names[fi]) != 0 || !ast_root) {
            fprintf(stderr, "Error: failed to parse package \"%s\"\n", import_path);
            package_source_free(&ps);
            return -1;
        }
        e->asts[fi] = ast_root;
        e->ast_count = fi + 1;   // keep in step so a later failure frees exactly
                                  // the slots already filled
        ast_root = NULL;
    }
    package_source_free(&ps);

    // Every file's own import list is walked: a package's dependencies are the
    // union of its files' imports, even though (from the next commit) each
    // file's import SCOPE stays its own.
    for (size_t fi = 0; fi < e->ast_count; fi++) {
        if (e->asts[fi]->type != AST_PROGRAM) continue;
        ProgramNode* prog = (ProgramNode*)e->asts[fi];
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
        bool ok = type_check_package(checker, p, e->asts, e->ast_count);

        // Forward-reference pre-pass ACROSS THE WHOLE PACKAGE, before any file's
        // bodies are emitted.
        //
        // codegen_generate_program runs this same pre-pass over the decls of the
        // one program it is given, which is what lets a call resolve a function
        // defined later in the SAME file (call sites bind via
        // LLVMGetNamedFunction). Across files that is not enough: emitting a.go
        // fully before b.go means a.go's bodies look up a prototype that b.go
        // has not created yet, and a genuine cross-file call fails codegen with
        // "Undefined identifier" even though it type-checked cleanly. Hoisting
        // the pre-pass over every file first is the codegen-level counterpart of
        // type_check_package running each pass over all files.
        //
        // Idempotent, so the per-file call inside codegen_generate_program is
        // harmless: codegen_predeclare_function guards on LLVMGetNamedFunction
        // and skips a symbol that already exists. Runs while current_package is
        // still set, which is what package-prefixes each symbol.
        for (size_t fi = 0; ok && fi < e->ast_count; fi++) {
            ProgramNode* fprog = (ProgramNode*)e->asts[fi];
            ok = codegen_predeclare_functions(codegen, checker, fprog->decls);
        }

        // Then emit each file, while the package scope from type_check_package
        // is still live (its LIFETIME CONTRACT).
        for (size_t fi = 0; ok && fi < e->ast_count; fi++) {
            ok = codegen_generate_program(codegen, checker, e->asts[fi]);
        }
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
        // The file-name array moves WITH the ASTs, never separately: the
        // Positions inside those ASTs hold these exact pointers.
        p->owned_asts = e->asts;
        p->owned_ast_count = e->ast_count;
        p->owned_file_names = e->file_names;
        p->owned_file_name_count = e->ast_count;
        e->asts = NULL;
        e->file_names = NULL;
        e->ast_count = 0;
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

// Is this one of a package's test files? The suffix set is Go's, extended with
// the .goo spelling — the same pair is_buildable_source gates on when
// `include_tests` is set (src/package/import_resolver.c), so the file set the
// resolver COLLECTS and the file set discovery SCANS cannot drift apart.
static bool is_test_source_name(const char* fname) {
    if (!fname) return false;
    size_t fl = strlen(fname);
    return (fl >= 8 && memcmp(fname + fl - 8, "_test.go", 8) == 0) ||
           (fl >= 9 && memcmp(fname + fl - 9, "_test.goo", 9) == 0);
}

static bool compile_file(const char* filename, CompilerOptions* options) {
    if (options->verbose) {
        printf("Compiling %s...\n", filename);
    }

    // P4.5: computed once, used by both the --dump-packages walk and the
    // real package-compilation walk below.
    // For a DIRECTORY argument the directory itself is the source dir, so a
    // sibling `import "./p"` inside it resolves relative to the package, not to
    // the cwd. For a file argument this is the file's directory, unchanged.
    char source_dir[4096];
    if (options->input_is_dir) {
        snprintf(source_dir, sizeof(source_dir), "%s", options->input_file);
    } else {
        compute_source_dir(filename, source_dir, sizeof(source_dir));
    }

    // Parse every file of the entry package. One file for `goo build x.goo`,
    // N for `goo build .` — the SAME path either way, so the long-standing
    // single-file form cannot drift away from the directory form.
    size_t nfiles = options->input_file_count;
    char**    sources = calloc(nfiles, sizeof(char*));
    Lexer**   lexers  = calloc(nfiles, sizeof(Lexer*));
    ASTNode** asts    = calloc(nfiles, sizeof(ASTNode*));
    if (!sources || !lexers || !asts) {
        free(sources); free(lexers); free(asts);
        fprintf(stderr, "Error: out of memory reading input files\n");
        return false;
    }

// Releases every per-file resource. One definition rather than the
// thirteen open-coded triples this function used to carry, each of which
// would otherwise have needed its own loop.
#define ENTRY_CLEANUP()                                            \
    do {                                                            \
        for (size_t _i = 0; _i < nfiles; _i++) {                    \
            if (asts[_i]) ast_node_free(asts[_i]);                  \
            if (lexers[_i]) lexer_free(lexers[_i]);                 \
            free(sources[_i]);                                      \
        }                                                           \
        free(asts); free(lexers); free(sources);                    \
    } while (0)

    for (size_t fi = 0; fi < nfiles; fi++) {
        const char* fname = options->input_files[fi];

        sources[fi] = read_file(fname);
        if (!sources[fi]) { ENTRY_CLEANUP(); return false; }

        if (options->verbose) printf("Phase 1: Lexical analysis (%s)...\n", fname);

        lexers[fi] = lexer_new(sources[fi], fname);
        if (!lexers[fi]) { ENTRY_CLEANUP(); return false; }

        if (options->emit_tokens) {
            printf("=== TOKENS (%s) ===\n", fname);
            Token* token;
            while ((token = lexer_next_token(lexers[fi])) && token->type != TOKEN_EOF) {
                printf("%-15s %s\n", token_type_string(token->type),
                       token->literal ? token->literal : "");
                token_free(token);
            }
            if (token) token_free(token);
            lexer_free(lexers[fi]);
            lexers[fi] = lexer_new(sources[fi], fname);
            if (!lexers[fi]) { ENTRY_CLEANUP(); return false; }
        }

        if (options->verbose) printf("Phase 2: Parsing (%s)...\n", fname);

        extern Lexer* current_lexer;
        current_lexer = lexers[fi];

        // ast_root is a bison global: snapshot into this file's slot and null
        // it, so the next file's parse cannot clobber a pointer we own.
        extern ASTNode* ast_root;
        ast_root = NULL;
        if (parse_input(sources[fi], fname) != 0) {
            fprintf(stderr, "Error: Parse failed\n");
            ENTRY_CLEANUP();
            return false;
        }
        if (!ast_root) {
            fprintf(stderr, "Error: No AST generated\n");
            ENTRY_CLEANUP();
            return false;
        }
        asts[fi] = ast_root;
        ast_root = NULL;

        if (options->emit_ast) {
            printf("=== AST (%s) ===\n", fname);
            ast_print(asts[fi], 0);
            printf("\n");
        }
    }

    // The entry package's first file stands in wherever a single AST is still
    // the right thing to pass (the import-graph dump below).
    ASTNode* ast = asts[0];

    // `goo test` on a package with no _test files: Go reports it and exits 0
    // rather than treating it as an error, so a whole-tree run is not derailed
    // by a package that simply has no tests yet.
    if (options->mode == GOO_MODE_TEST) {
        int have_test_file = 0;
        for (size_t fi = 0; fi < nfiles && !have_test_file; fi++) {
            have_test_file = is_test_source_name(options->input_files[fi]);
        }
        if (!have_test_file) {
            // Named after the entry directory, the same string the output stem
            // uses; for a single-file argument, that file's own name.
            char stem[PATH_MAX];
            const char* src = options->input_file;
            char resolved[PATH_MAX];
            if (options->input_is_dir && realpath(options->input_file, resolved)) src = resolved;
            const char* slash = strrchr(src, '/');
            snprintf(stem, sizeof(stem), "%s", (slash && slash[1]) ? slash + 1 : src);
            printf("?   %s  [no test files]\n", stem);
            // No binary was emitted, so there is nothing to exec. Without
            // this, main() sees success && run_after_compile and tries to run
            // the empty file mkstemp created for the temp output path
            // ("Permission denied", exit 127).
            options->run_after_compile = false;
            ENTRY_CLEANUP();
            return true;
        }

        // Collect the TestXxx functions, and reject a Test-named function of
        // the wrong shape. Runs BEFORE type_checker_new() because Task 6's
        // synthesized _testmain.goo has to join `asts` before the marker-
        // seeding loop below walks it.
        //
        // Only the _test files are scanned, which is both Go's rule and a
        // consistency requirement: scanning an ordinary package file would
        // reject a `func TestHelper(x int)` that `goo build` accepts, so the
        // same source would compile or fail depending on the subcommand.
        ASTNode** test_asts = calloc(nfiles, sizeof(ASTNode*));
        const char** test_names = calloc(nfiles, sizeof(char*));
        if (!test_asts || !test_names) {
            free(test_asts); free(test_names);
            fprintf(stderr, "Error: out of memory preparing test discovery\n");
            ENTRY_CLEANUP();
            return false;
        }
        size_t test_file_count = 0;
        for (size_t fi = 0; fi < nfiles; fi++) {
            if (!is_test_source_name(options->input_files[fi])) continue;
            test_asts[test_file_count] = asts[fi];
            test_names[test_file_count] = options->input_files[fi];
            test_file_count++;
        }

        TestList tests = {0};
        int discovered = test_discovery_collect(test_asts, test_file_count,
                                                test_names, &tests);
        free(test_asts);
        free(test_names);
        if (!discovered) {
            // The diagnostic is already on stderr. Leave no binary behind and
            // exit non-zero, the same discipline every other reject path keeps.
            options->run_after_compile = false;
            ENTRY_CLEANUP();
            return false;
        }
        // A `package main` PROGRAM under test already declares `main`, and the
        // synthesized file is about to declare another one in the same package
        // — "Function 'main' already declared". Every CLI tool is package main,
        // so without this no CLI tool could have tests at all.
        //
        // Rename the program's own main so the synthesized entry point is the
        // only one. This matches what `go test` DOES for package main: the test
        // binary's entry is the generated main, and the program's main is
        // compiled but never called. Go achieves that by compiling the package
        // as a library and putting its testmain in a separate package; renaming
        // reaches the same observable behaviour without the package-architecture
        // change, which is the trade recorded here deliberately.
        //
        // The chosen name is not a legal Goo identifier, so it can never
        // collide with a user declaration.
        //
        // A test calling `main()` by name resolves to the SYNTHESIZED entry
        // rather than the program's own, which Go would not do. That costs
        // nothing observable, because calling `main()` is already broken in
        // Goo everywhere: it fails module verification with "Incorrect number
        // of arguments passed to called function" in an ORDINARY build too,
        // since codegen gives the entry an i32 return that no call site
        // expects. Verified against a plain `goo build` with no test mode
        // involved — a pre-existing defect this rename neither causes nor
        // worsens. Ledgered separately.
        for (size_t fi = 0; fi < nfiles; fi++) {
            if (!asts[fi] || asts[fi]->type != AST_PROGRAM) continue;
            for (ASTNode* d = ((ProgramNode*)asts[fi])->decls; d; d = d->next) {
                if (d->type != AST_FUNC_DECL) continue;
                FuncDeclNode* f = (FuncDeclNode*)d;
                if (!f->name || f->receiver || strcmp(f->name, "main") != 0) continue;
                char* renamed = str_dup("main.program");
                if (!renamed) {
                    fprintf(stderr, "Error: out of memory preparing the test entry point\n");
                    ENTRY_CLEANUP();
                    return false;
                }
                free(f->name);
                f->name = renamed;
            }
        }

        // Synthesize _testmain.goo and parse it as ONE MORE FILE of the entry
        // package — the operation PR #226 made ordinary. It is never written to
        // disk; --emit-testmain prints it.
        //
        // This must happen before type_checker_new() below, because the
        // marker-seeding loop walks every asts[] entry for its imports and the
        // generated file is what imports "testing".
        const char* pkg_name = ((ProgramNode*)asts[0])->package_name;
        char* testmain_src = test_discovery_build_main(pkg_name ? pkg_name : "main",
                                                       &tests);
        test_list_free(&tests);
        if (!testmain_src) {
            fprintf(stderr, "Error: out of memory generating _testmain.goo\n");
            ENTRY_CLEANUP();
            return false;
        }

        if (options->emit_testmain) {
            fputs(testmain_src, stdout);
            free(testmain_src);
            options->run_after_compile = false;
            ENTRY_CLEANUP();
            return true;
        }

        // Grow the three parallel arrays by one. On a realloc failure the
        // ORIGINAL pointer is still live, so assign through a temporary —
        // writing straight into sources/lexers/asts would lose every file
        // already parsed and leak all of it.
        char** grown_sources = realloc(sources, (nfiles + 1) * sizeof(char*));
        if (grown_sources) sources = grown_sources;
        Lexer** grown_lexers = grown_sources ? realloc(lexers, (nfiles + 1) * sizeof(Lexer*)) : NULL;
        if (grown_lexers) lexers = grown_lexers;
        ASTNode** grown_asts = grown_lexers ? realloc(asts, (nfiles + 1) * sizeof(ASTNode*)) : NULL;
        if (grown_asts) asts = grown_asts;
        if (!grown_asts) {
            free(testmain_src);
            fprintf(stderr, "Error: out of memory generating _testmain.goo\n");
            ENTRY_CLEANUP();
            return false;
        }

        // The source buffer joins `sources` so ENTRY_CLEANUP frees it. No
        // lexer: parse_input builds its own, which is why the package path
        // (parse of an imported package's files) sets none either.
        sources[nfiles] = testmain_src;
        lexers[nfiles]  = NULL;
        asts[nfiles]    = NULL;

        // A string literal has static storage duration, so it outlives every
        // Position that parse_input stamps this pointer into. The real files
        // must strdup their names because those are built at runtime.
        static const char kTestMainName[] = "_testmain.goo";

        extern ASTNode* ast_root;
        ast_root = NULL;
        if (parse_input(testmain_src, kTestMainName) != 0 || !ast_root) {
            // Generated source that will not parse is a compiler bug, not a
            // user error, so point at the flag that shows the text.
            fprintf(stderr, "Error: failed to parse the synthesized _testmain.goo "
                            "(re-run with --emit-testmain to see it)\n");
            nfiles++;   // the slot is owned now; let ENTRY_CLEANUP free it
            ENTRY_CLEANUP();
            return false;
        }
        asts[nfiles] = ast_root;
        ast_root = NULL;
        nfiles++;

        if (options->emit_ast) {
            printf("=== AST (%s) ===\n", kTestMainName);
            ast_print(asts[nfiles - 1], 0);
            printf("\n");
        }
    }

    // Hidden debug flag: walk the import graph from main and print the
    // resolved packages in topological order (leaves first), then "main".
    // Short-circuits before type-checking/codegen — packages are not yet
    // fed into later phases (Tasks 4/5). `ast` (main) was snapshotted above,
    // so the walk clobbering global ast_root is harmless.
    if (options->dump_packages) {
        bool ok = dump_package_graph((ProgramNode*)ast, source_dir);
        ENTRY_CLEANUP();
        return ok;
    }

    // Phase 3: Type Checking
    if (options->verbose) {
        printf("Phase 3: Type checking...\n");
    }
    
    TypeChecker* type_checker = type_checker_new();
    if (!type_checker) {
        ENTRY_CLEANUP();
        return false;
    }
    
    // stdlib Phase 0 (Task 4): seed TYPE_PACKAGE markers for the stdlib-shim
    // packages main actually imports, BEFORE type-checking main (so its
    // `fmt.Println` etc. resolve). Conditional on real imports — a no-import
    // program seeds nothing and type-checks exactly as before.
    bool markers_ok = true;
    for (size_t fi = 0; markers_ok && fi < nfiles; fi++) {
        markers_ok = seed_imported_stdlib_markers(type_checker,
                                                  ((ProgramNode*)asts[fi])->imports);
    }
    if (!markers_ok) {
        type_checker_free(type_checker);
        ENTRY_CLEANUP();
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
        ENTRY_CLEANUP();
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
        bool pkgs_ok = true;
        for (size_t fi = 0; pkgs_ok && fi < nfiles; fi++) {
            pkgs_ok = (walk_program_imports(&pkg_graph,
                                            ((ProgramNode*)asts[fi])->imports) == 0);
        }
        pkgs_ok = pkgs_ok
            && compile_resolved_packages(&pkg_graph, type_checker, codegen);
        pkg_graph_free(&pkg_graph);
        if (!pkgs_ok) {
            codegen_free(codegen);
            type_checker_free(type_checker);
            ENTRY_CLEANUP();
            return false;
        }
    }
#endif

    // Phase 3: Type Checking (main). Runs AFTER package compilation so that a
    // cross-package selector like `mypkg.Double(21)` resolves against the real
    // exported signature published into pkg->exports above. With no non-shim
    // imports the packages block is a no-op and this stays byte-identical.
    if (!type_check_program_files(type_checker, asts, nfiles)) {
#if LLVM_AVAILABLE
        codegen_free(codegen);
#endif
        type_checker_free(type_checker);
        ENTRY_CLEANUP();
        return false;
    }

    // Phase 4: Code Generation
    if (options->verbose) {
        printf("Phase 4: Code generation...\n");
    }

#if LLVM_AVAILABLE
    // Generate code for main
    bool main_cg_ok = true;
    for (size_t fi = 0; main_cg_ok && fi < nfiles; fi++) {
        main_cg_ok = codegen_predeclare_functions(codegen, type_checker,
                                                  ((ProgramNode*)asts[fi])->decls);
    }
    for (size_t fi = 0; main_cg_ok && fi < nfiles; fi++) {
        main_cg_ok = codegen_generate_program(codegen, type_checker, asts[fi]);
    }
    if (!main_cg_ok) {
        codegen_free(codegen);
        type_checker_free(type_checker);
        ENTRY_CLEANUP();
        return false;
    }

    // P3.10: run the new-PM optimization pipeline (no-op at opt_level 0 —
    // byte-identical contract) BEFORE either emit path, so --emit-llvm shows
    // the optimized IR too — that's how the differential (-O0 vs -O2 IR)
    // gate observes optimization actually happening.
    if (!codegen_optimize(codegen, options->opt_level)) {
        codegen_free(codegen);
        type_checker_free(type_checker);
        ENTRY_CLEANUP();
        return false;
    }

    // P5.2: --emit-llvm writes the textual IR to the output path itself and
    // produces NO executable (pre-fix, an always-true conditional wrote the
    // ELF to the -o path and the IR to <path>.ll).
    if (options->emit_llvm_ir) {
        if (!codegen_emit_llvm_ir(codegen, options->output_file)) {
            codegen_free(codegen);
            type_checker_free(type_checker);
            ENTRY_CLEANUP();
            return false;
        }
        if (options->verbose) {
            printf("LLVM IR written to: %s\n", options->output_file);
        }
    } else {
        if (!codegen_emit_executable(codegen, options->output_file)) {
            codegen_free(codegen);
            type_checker_free(type_checker);
            ENTRY_CLEANUP();
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
    ENTRY_CLEANUP();
#undef ENTRY_CLEANUP

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