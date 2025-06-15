#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

typedef enum {
    TEMPLATE_WEB_API,
    TEMPLATE_CLI_TOOL,
    TEMPLATE_MICROSERVICE,
    TEMPLATE_LIBRARY,
    TEMPLATE_GAME,
    TEMPLATE_COUNT
} TemplateType;

typedef struct {
    TemplateType type;
    const char* name;
    const char* description;
    const char* directory;
} Template;

static Template templates[] = {
    {TEMPLATE_WEB_API, "web-api", "REST API server with error unions and JSON handling", "web_api"},
    {TEMPLATE_CLI_TOOL, "cli-tool", "Command-line application with safe argument parsing", "cli_tool"},
    {TEMPLATE_MICROSERVICE, "microservice", "Production microservice with observability", "microservice"},
    {TEMPLATE_LIBRARY, "library", "Reusable library with comprehensive testing", "library"},
    {TEMPLATE_GAME, "game", "Game development with ECS and safe concurrency", "game"}
};

void print_banner() {
    printf("🎯 Goo Project Template Wizard\n");
    printf("===============================\n\n");
    printf("Create new Goo projects with best practices built-in!\n\n");
}

void print_templates() {
    printf("📋 Available Templates:\n\n");
    
    for (int i = 0; i < TEMPLATE_COUNT; i++) {
        printf("  %d. %-15s - %s\n", i + 1, templates[i].name, templates[i].description);
    }
    printf("\n");
}

int get_user_choice() {
    int choice;
    printf("Select template (1-%d): ", TEMPLATE_COUNT);
    
    if (scanf("%d", &choice) != 1) {
        printf("❌ Invalid input. Please enter a number.\n");
        return -1;
    }
    
    if (choice < 1 || choice > TEMPLATE_COUNT) {
        printf("❌ Invalid choice. Please select 1-%d.\n", TEMPLATE_COUNT);
        return -1;
    }
    
    return choice - 1;
}

char* get_project_name() {
    static char project_name[256];
    printf("Enter project name: ");
    
    if (scanf("%255s", project_name) != 1) {
        printf("❌ Failed to read project name.\n");
        return NULL;
    }
    
    // Validate project name (alphanumeric, hyphens, underscores)
    for (int i = 0; project_name[i]; i++) {
        char c = project_name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            printf("❌ Invalid project name. Use only letters, numbers, hyphens, and underscores.\n");
            return NULL;
        }
    }
    
    return project_name;
}

int directory_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int create_directory(const char* path) {
    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) {
            printf("❌ Directory '%s' already exists.\n", path);
            return 0;
        }
        printf("❌ Failed to create directory '%s': %s\n", path, strerror(errno));
        return 0;
    }
    return 1;
}

int copy_file(const char* src, const char* dest) {
    FILE* source = fopen(src, "rb");
    if (!source) {
        printf("❌ Failed to open source file '%s': %s\n", src, strerror(errno));
        return 0;
    }
    
    FILE* destination = fopen(dest, "wb");
    if (!destination) {
        printf("❌ Failed to create destination file '%s': %s\n", dest, strerror(errno));
        fclose(source);
        return 0;
    }
    
    char buffer[4096];
    size_t bytes;
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, bytes, destination) != bytes) {
            printf("❌ Failed to write to '%s': %s\n", dest, strerror(errno));
            fclose(source);
            fclose(destination);
            return 0;
        }
    }
    
    fclose(source);
    fclose(destination);
    return 1;
}

int copy_template(const char* template_dir, const char* project_name) {
    char template_path[512];
    char project_path[512];
    char src_file[512];
    char dest_file[512];
    
    // Get the directory where this executable is located
    char exe_path[512];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        // Fallback to current directory
        strcpy(exe_path, ".");
    } else {
        exe_path[len] = '\0';
        // Remove the executable name to get directory
        char* last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\\0';
        }
    }
    
    // Build template path (relative to project root)
    snprintf(template_path, sizeof(template_path), "%s/../../templates/%s", exe_path, template_dir);
    
    // Create project directory
    if (!create_directory(project_name)) {
        return 0;
    }
    
    printf("📁 Copying template files...\n");
    
    // Copy main.goo
    snprintf(src_file, sizeof(src_file), "%s/main.goo", template_path);
    snprintf(dest_file, sizeof(dest_file), "%s/main.goo", project_name);
    
    if (!copy_file(src_file, dest_file)) {
        printf("⚠️  Could not copy main.goo (template may be incomplete)\n");
    } else {
        printf("  ✅ main.goo\n");
    }
    
    // Copy README.md
    snprintf(src_file, sizeof(src_file), "%s/README.md", template_path);
    snprintf(dest_file, sizeof(dest_file), "%s/README.md", project_name);
    
    if (!copy_file(src_file, dest_file)) {
        printf("⚠️  Could not copy README.md (template may be incomplete)\n");
    } else {
        printf("  ✅ README.md\n");
    }
    
    // Create basic project structure
    snprintf(project_path, sizeof(project_path), "%s/src", project_name);
    if (create_directory(project_path)) {
        printf("  ✅ src/\n");
    }
    
    snprintf(project_path, sizeof(project_path), "%s/tests", project_name);
    if (create_directory(project_path)) {
        printf("  ✅ tests/\n");
    }
    
    snprintf(project_path, sizeof(project_path), "%s/docs", project_name);
    if (create_directory(project_path)) {
        printf("  ✅ docs/\n");
    }
    
    return 1;
}

void create_project_files(const char* project_name, const Template* template) {
    char filepath[512];
    FILE* file;
    
    // Create go.mod equivalent (goo.mod)
    snprintf(filepath, sizeof(filepath), "%s/goo.mod", project_name);
    file = fopen(filepath, "w");
    if (file) {
        fprintf(file, "module %s\n\n", project_name);
        fprintf(file, "goo 1.0\n\n");
        fprintf(file, "require (\n");
        fprintf(file, "    // Add dependencies here\n");
        fprintf(file, ")\n");
        fclose(file);
        printf("  ✅ goo.mod\n");
    }
    
    // Create .gitignore
    snprintf(filepath, sizeof(filepath), "%s/.gitignore", project_name);
    file = fopen(filepath, "w");
    if (file) {
        fprintf(file, "# Goo build artifacts\n");
        fprintf(file, "*.o\n");
        fprintf(file, "*.a\n");
        fprintf(file, "*.so\n");
        fprintf(file, "/bin/\n");
        fprintf(file, "/build/\n");
        fprintf(file, "/dist/\n\n");
        fprintf(file, "# IDE files\n");
        fprintf(file, ".vscode/\n");
        fprintf(file, ".idea/\n");
        fprintf(file, "*.swp\n");
        fprintf(file, "*.swo\n\n");
        fprintf(file, "# OS files\n");
        fprintf(file, ".DS_Store\n");
        fprintf(file, "Thumbs.db\n");
        fclose(file);
        printf("  ✅ .gitignore\n");
    }
    
    // Create Makefile
    snprintf(filepath, sizeof(filepath), "%s/Makefile", project_name);
    file = fopen(filepath, "w");
    if (file) {
        fprintf(file, ".PHONY: build test clean run\n\n");
        fprintf(file, "build:\n");
        fprintf(file, "\\tgoo build -o bin/%s main.goo\n\n", project_name);
        fprintf(file, "test:\n");
        fprintf(file, "\\tgoo test ./...\n\n");
        fprintf(file, "run: build\n");
        fprintf(file, "\\t./bin/%s\n\n", project_name);
        fprintf(file, "clean:\n");
        fprintf(file, "\\trm -rf bin/ build/ dist/\n\n");
        fprintf(file, "install: build\n");
        fprintf(file, "\\tcp bin/%s /usr/local/bin/\n", project_name);
        fclose(file);
        printf("  ✅ Makefile\n");
    }
}

void print_next_steps(const char* project_name, const Template* template) {
    printf("\n🎉 Project '%s' created successfully!\n\n", project_name);
    printf("📚 Template: %s - %s\n\n", template->name, template->description);
    
    printf("🚀 Next Steps:\n");
    printf("  1. cd %s\n", project_name);
    printf("  2. cat README.md           # Read the template documentation\n");
    printf("  3. make build              # Build the project\n");
    printf("  4. make run                # Run the application\n");
    printf("  5. make test               # Run tests\n\n");
    
    printf("📖 Learn More:\n");
    printf("  • Template documentation: %s/README.md\n", project_name);
    printf("  • Goo language guide: https://goo-lang.org/docs\n");
    printf("  • Community examples: https://github.com/goo-lang/examples\n\n");
    
    printf("💡 Pro Tips:\n");
    printf("  • Use 'goo fmt' to format your code\n");
    printf("  • Use 'goo vet' to check for common mistakes\n");
    printf("  • Use 'goo doc' to generate documentation\n");
    printf("  • Initialize git: 'git init && git add . && git commit -m \"Initial commit\"'\n\n");
    
    printf("Happy coding with Goo! 🎯✨\n");
}

int main(int argc, char* argv[]) {
    print_banner();
    
    // Check if template and project name provided as arguments
    if (argc == 3) {
        const char* template_name = argv[1];
        const char* project_name = argv[2];
        
        // Find template by name
        Template* selected_template = NULL;
        for (int i = 0; i < TEMPLATE_COUNT; i++) {
            if (strcmp(templates[i].name, template_name) == 0) {
                selected_template = &templates[i];
                break;
            }
        }
        
        if (!selected_template) {
            printf("❌ Unknown template '%s'\n\n", template_name);
            print_templates();
            return 1;
        }
        
        printf("📋 Creating project '%s' from template '%s'\n\n", project_name, template_name);
        
        if (!copy_template(selected_template->directory, project_name)) {
            return 1;
        }
        
        create_project_files(project_name, selected_template);
        print_next_steps(project_name, selected_template);
        return 0;
    }
    
    // Interactive mode
    print_templates();
    
    int choice = get_user_choice();
    if (choice == -1) {
        return 1;
    }
    
    char* project_name = get_project_name();
    if (!project_name) {
        return 1;
    }
    
    Template* selected_template = &templates[choice];
    
    printf("\n📋 Creating project '%s' from template '%s'\n", project_name, selected_template->name);
    printf("🔧 Template: %s\n\n", selected_template->description);
    
    if (!copy_template(selected_template->directory, project_name)) {
        return 1;
    }
    
    create_project_files(project_name, selected_template);
    print_next_steps(project_name, selected_template);
    
    return 0;
}