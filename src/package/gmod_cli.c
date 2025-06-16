#include "package/goo_mod.h"
#include "package/package_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

// CLI command structure
typedef struct {
    const char* name;
    const char* description;
    int (*handler)(int argc, char** argv);
} Command;

// Global options
static bool verbose = false;
static bool offline = false;
static bool force = false;
static char* workspace = NULL;

// Forward declarations
int cmd_init(int argc, char** argv);
int cmd_add(int argc, char** argv);
int cmd_remove(int argc, char** argv);
int cmd_update(int argc, char** argv);
int cmd_install(int argc, char** argv);
int cmd_list(int argc, char** argv);
int cmd_search(int argc, char** argv);
int cmd_suggest(int argc, char** argv);
int cmd_optimize(int argc, char** argv);
int cmd_audit(int argc, char** argv);
int cmd_analyze(int argc, char** argv);
int cmd_graph(int argc, char** argv);
int cmd_clean(int argc, char** argv);
int cmd_doctor(int argc, char** argv);
int cmd_version(int argc, char** argv);
int cmd_help(int argc, char** argv);

// Command registry
static Command commands[] = {
    {"init", "Initialize a new Goo project", cmd_init},
    {"add", "Add a dependency to the project", cmd_add},
    {"remove", "Remove a dependency from the project", cmd_remove},
    {"update", "Update dependencies", cmd_update},
    {"install", "Install all dependencies", cmd_install},
    {"list", "List project dependencies", cmd_list},
    {"search", "Search for packages", cmd_search},
    {"suggest", "AI-suggested dependencies", cmd_suggest},
    {"optimize", "Optimize dependency tree", cmd_optimize},
    {"audit", "Security audit of dependencies", cmd_audit},
    {"analyze", "Analyze dependency impact", cmd_analyze},
    {"graph", "Visualize dependency graph", cmd_graph},
    {"clean", "Clean package cache", cmd_clean},
    {"doctor", "Check project health", cmd_doctor},
    {"version", "Show version information", cmd_version},
    {"help", "Show help information", cmd_help},
    {NULL, NULL, NULL}
};

// Utility functions
void print_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "\033[31mError:\033[0m ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void print_warning(const char* format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "\033[33mWarning:\033[0m ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void print_success(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf("\033[32m✓\033[0m ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

void print_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf("\033[34mℹ\033[0m ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

GooMod* load_project_module(void) {
    // Try to find goo.mod in current directory or parent directories
    char* current_dir = getcwd(NULL, 0);
    char filepath[1024];
    
    for (int i = 0; i < 10; i++) { // Limit search depth
        snprintf(filepath, sizeof(filepath), "%s/goo.mod", current_dir);
        
        if (file_exists(filepath)) {
            GooMod* gmod = goo_mod_parse_file(filepath);
            free(current_dir);
            return gmod;
        }
        
        // Move up one directory
        char* parent = strrchr(current_dir, '/');
        if (parent) {
            *parent = '\0';
        } else {
            break;
        }
    }
    
    free(current_dir);
    return NULL;
}

// Command implementations

int cmd_init(int argc, char** argv) {
    const char* name = NULL;
    const char* template_name = "basic";
    bool workspace = false;
    
    // Parse options
    int opt;
    static struct option long_options[] = {
        {"name", required_argument, 0, 'n'},
        {"template", required_argument, 0, 't'},
        {"workspace", no_argument, 0, 'w'},
        {0, 0, 0, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "n:t:w", long_options, NULL)) != -1) {
        switch (opt) {
            case 'n':
                name = optarg;
                break;
            case 't':
                template_name = optarg;
                break;
            case 'w':
                workspace = true;
                break;
            default:
                return 1;
        }
    }
    
    // Get project name from argument or use current directory name
    if (!name && optind < argc) {
        name = argv[optind];
    }
    
    if (!name) {
        char* cwd = getcwd(NULL, 0);
        name = strrchr(cwd, '/');
        if (name) name++;
        else name = cwd;
    }
    
    print_info("Initializing Goo project: %s", name);
    
    // Check if goo.mod already exists
    if (file_exists("goo.mod")) {
        print_error("goo.mod already exists in this directory");
        return 1;
    }
    
    // Create basic goo.mod
    GooMod* gmod = goo_mod_create("", "0.1.0");
    if (!gmod) {
        print_error("Failed to create module");
        return 1;
    }
    
    // Set basic properties
    free(gmod->module_path);
    gmod->module_path = string_duplicate(name);
    gmod->description = string_duplicate("A new Goo project");
    gmod->license = string_duplicate("MIT");
    gmod->goo_version = string_duplicate(">=0.1.0");
    
    // Create default directories
    if (!directory_exists("src")) {
        if (mkdir("src", 0755) != 0) {
            print_warning("Failed to create src directory");
        }
    }
    
    // Create main.goo if it doesn't exist
    if (!file_exists("src/main.goo")) {
        const char* main_content = 
            "// Package main\n"
            "package main\n"
            "\n"
            "import \"fmt\"\n"
            "\n"
            "func main() {\n"
            "    fmt.Println(\"Hello, World!\")\n"
            "}\n";
        
        write_file_contents("src/main.goo", main_content);
    }
    
    // Save goo.mod
    if (!goo_mod_save_file(gmod, "goo.mod")) {
        print_error("Failed to create goo.mod");
        goo_mod_free(gmod);
        return 1;
    }
    
    goo_mod_free(gmod);
    
    print_success("Created project %s", name);
    print_info("Edit goo.mod to configure your project");
    print_info("Run 'gmod add <package>' to add dependencies");
    
    return 0;
}

int cmd_add(int argc, char** argv) {
    if (optind >= argc) {
        print_error("Package name required");
        printf("Usage: gmod add <package>[@version] [--dev] [--feature=<feature>]\n");
        return 1;
    }
    
    const char* package_spec = argv[optind];
    bool dev_dependency = false;
    const char* feature = NULL;
    
    // Parse options
    int opt;
    static struct option long_options[] = {
        {"dev", no_argument, 0, 'd'},
        {"feature", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "df:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                dev_dependency = true;
                break;
            case 'f':
                feature = optarg;
                break;
            default:
                return 1;
        }
    }
    
    // Parse package specification
    char* package_name = string_duplicate(package_spec);
    char* version_constraint = NULL;
    
    char* at_sign = strchr(package_name, '@');
    if (at_sign) {
        *at_sign = '\0';
        version_constraint = string_duplicate(at_sign + 1);
    } else {
        version_constraint = string_duplicate("*");
    }
    
    print_info("Adding %s dependency: %s@%s", 
               dev_dependency ? "dev" : "runtime", 
               package_name, version_constraint);
    
    // Load current module
    GooMod* gmod = load_project_module();
    if (!gmod) {
        print_error("No goo.mod found. Run 'gmod init' first.");
        free(package_name);
        free(version_constraint);
        return 1;
    }
    
    // Create new dependency
    Dependency* new_dep = dependency_create(package_name, version_constraint);
    if (!new_dep) {
        print_error("Failed to create dependency");
        goo_mod_free(gmod);
        free(package_name);
        free(version_constraint);
        return 1;
    }
    
    // Add to appropriate dependency list
    if (dev_dependency) {
        new_dep->next = gmod->dev_dependencies;
        gmod->dev_dependencies = new_dep;
    } else {
        new_dep->next = gmod->dependencies;
        gmod->dependencies = new_dep;
    }
    
    // Save updated module
    if (!goo_mod_save_file(gmod, "goo.mod")) {
        print_error("Failed to save goo.mod");
        goo_mod_free(gmod);
        free(package_name);
        free(version_constraint);
        return 1;
    }
    
    print_success("Added dependency: %s@%s", package_name, version_constraint);
    print_info("Run 'gmod install' to install dependencies");
    
    goo_mod_free(gmod);
    free(package_name);
    free(version_constraint);
    return 0;
}

int cmd_list(int argc, char** argv) {
    bool tree_view = false;
    bool show_dev = false;
    
    // Parse options
    int opt;
    static struct option long_options[] = {
        {"tree", no_argument, 0, 't'},
        {"dev", no_argument, 0, 'd'},
        {0, 0, 0, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "td", long_options, NULL)) != -1) {
        switch (opt) {
            case 't':
                tree_view = true;
                break;
            case 'd':
                show_dev = true;
                break;
            default:
                return 1;
        }
    }
    
    GooMod* gmod = load_project_module();
    if (!gmod) {
        print_error("No goo.mod found");
        return 1;
    }
    
    printf("Dependencies for %s@", gmod->module_path ? gmod->module_path : "unknown");
    if (gmod->version) {
        char* version_str = version_to_string(gmod->version);
        printf("%s", version_str);
        free(version_str);
    }
    printf(":\n\n");
    
    // List runtime dependencies
    if (gmod->dependencies) {
        printf("Runtime dependencies:\n");
        for (Dependency* dep = gmod->dependencies; dep; dep = dep->next) {
            char* constraint_str = version_constraint_to_string(dep->constraint);
            printf("  %s@%s\n", dep->name, constraint_str);
            free(constraint_str);
        }
        printf("\n");
    }
    
    // List dev dependencies if requested
    if (show_dev && gmod->dev_dependencies) {
        printf("Development dependencies:\n");
        for (Dependency* dep = gmod->dev_dependencies; dep; dep = dep->next) {
            char* constraint_str = version_constraint_to_string(dep->constraint);
            printf("  %s@%s\n", dep->name, constraint_str);
            free(constraint_str);
        }
        printf("\n");
    }
    
    goo_mod_free(gmod);
    return 0;
}

int cmd_suggest(int argc, char** argv) {
    print_info("AI-powered dependency suggestions");
    
    GooMod* gmod = load_project_module();
    if (!gmod) {
        print_error("No goo.mod found");
        return 1;
    }
    
    printf("Based on your project, you might want to consider:\n\n");
    
    // Mock suggestions - in real implementation, this would use AI
    printf("📦 \033[36mhttp\033[0m - HTTP client and server library\n");
    printf("   Reason: Common for web applications\n");
    printf("   Command: gmod add http\n\n");
    
    printf("📦 \033[36mjson\033[0m - JSON parsing and serialization\n");
    printf("   Reason: Frequently used with HTTP APIs\n");
    printf("   Command: gmod add json\n\n");
    
    printf("📦 \033[36mtest\033[0m - Testing framework\n");
    printf("   Reason: Essential for reliable code\n");
    printf("   Command: gmod add test --dev\n\n");
    
    goo_mod_free(gmod);
    return 0;
}

int cmd_doctor(int argc, char** argv) {
    print_info("Running project health check...\n");
    
    bool all_good = true;
    
    // Check for goo.mod
    if (file_exists("goo.mod")) {
        print_success("goo.mod found");
        
        GooMod* gmod = goo_mod_parse_file("goo.mod");
        if (gmod) {
            print_success("goo.mod is valid");
            
            char* error_msg = NULL;
            if (module_validate((Module*)gmod, &error_msg)) {
                print_success("Module validation passed");
            } else {
                print_error("Module validation failed: %s", error_msg);
                free(error_msg);
                all_good = false;
            }
            
            goo_mod_free(gmod);
        } else {
            print_error("goo.mod is invalid");
            all_good = false;
        }
    } else {
        print_error("goo.mod not found");
        all_good = false;
    }
    
    // Check for source directory
    if (directory_exists("src")) {
        print_success("Source directory found");
    } else {
        print_warning("No src directory found");
    }
    
    // Check for main file
    if (file_exists("src/main.goo")) {
        print_success("Main file found");
    } else {
        print_info("No main.goo found (library project?)");
    }
    
    // Check Goo compiler
    if (system("which goo > /dev/null 2>&1") == 0) {
        print_success("Goo compiler found");
    } else {
        print_error("Goo compiler not found in PATH");
        all_good = false;
    }
    
    printf("\n");
    if (all_good) {
        print_success("Project health check passed");
        return 0;
    } else {
        print_error("Project health check failed");
        return 1;
    }
}

int cmd_version(int argc, char** argv) {
    printf("gmod 0.1.0 - Goo Package Manager\n");
    printf("The next-generation package manager for Goo\n\n");
    printf("Features:\n");
    printf("  • Compile-time dependency resolution\n");
    printf("  • AI-enhanced optimization\n");
    printf("  • Security-first design\n");
    printf("  • Zero-cost abstractions\n\n");
    printf("For more information, visit: https://goo-lang.org\n");
    return 0;
}

int cmd_help(int argc, char** argv) {
    printf("gmod - Goo Package Manager\n\n");
    printf("USAGE:\n");
    printf("    gmod [OPTIONS] <COMMAND> [ARGS]\n\n");
    printf("OPTIONS:\n");
    printf("    -v, --verbose     Enable verbose output\n");
    printf("    -o, --offline     Work offline\n");
    printf("    -f, --force       Force operation\n");
    printf("    -w, --workspace   Operate on workspace\n");
    printf("    -h, --help        Show help\n\n");
    printf("COMMANDS:\n");
    
    for (Command* cmd = commands; cmd->name; cmd++) {
        printf("    %-12s %s\n", cmd->name, cmd->description);
    }
    
    printf("\nFor more help on a specific command, use: gmod help <command>\n");
    return 0;
}

// Placeholder implementations for remaining commands
int cmd_remove(int argc, char** argv) {
    print_info("Remove command not yet implemented");
    return 1;
}

int cmd_update(int argc, char** argv) {
    print_info("Update command not yet implemented");
    return 1;
}

int cmd_install(int argc, char** argv) {
    print_info("Install command not yet implemented");
    return 1;
}

int cmd_search(int argc, char** argv) {
    print_info("Search command not yet implemented");
    return 1;
}

int cmd_optimize(int argc, char** argv) {
    print_info("Optimize command not yet implemented");
    return 1;
}

int cmd_audit(int argc, char** argv) {
    print_info("Audit command not yet implemented");
    return 1;
}

int cmd_analyze(int argc, char** argv) {
    print_info("Analyze command not yet implemented");
    return 1;
}

int cmd_graph(int argc, char** argv) {
    print_info("Graph command not yet implemented");
    return 1;
}

int cmd_clean(int argc, char** argv) {
    print_info("Clean command not yet implemented");
    return 1;
}

// Main function
int main(int argc, char** argv) {
    // Parse global options
    int opt;
    static struct option long_options[] = {
        {"verbose", no_argument, 0, 'v'},
        {"offline", no_argument, 0, 'o'},
        {"force", no_argument, 0, 'f'},
        {"workspace", required_argument, 0, 'w'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "vofhw:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'v':
                verbose = true;
                break;
            case 'o':
                offline = true;
                break;
            case 'f':
                force = true;
                break;
            case 'w':
                workspace = string_duplicate(optarg);
                break;
            case 'h':
                return cmd_help(argc, argv);
            default:
                return 1;
        }
    }
    
    // Get command
    if (optind >= argc) {
        printf("No command specified. Use 'gmod help' for usage.\n");
        return 1;
    }
    
    const char* command_name = argv[optind];
    
    // Find and execute command
    for (Command* cmd = commands; cmd->name; cmd++) {
        if (strcmp(cmd->name, command_name) == 0) {
            return cmd->handler(argc, argv);
        }
    }
    
    print_error("Unknown command: %s", command_name);
    printf("Use 'gmod help' to see available commands.\n");
    return 1;
}