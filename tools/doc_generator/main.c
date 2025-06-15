#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

typedef struct {
    char name[256];
    char description[1024];
    char signature[512];
    char example[2048];
    char file_path[512];
    int line_number;
} DocEntry;

typedef struct {
    DocEntry* entries;
    int count;
    int capacity;
} Documentation;

void init_documentation(Documentation* docs) {
    docs->capacity = 100;
    docs->entries = malloc(sizeof(DocEntry) * docs->capacity);
    docs->count = 0;
}

void add_doc_entry(Documentation* docs, const char* name, const char* description, 
                  const char* signature, const char* example, const char* file_path, int line) {
    if (docs->count >= docs->capacity) {
        docs->capacity *= 2;
        docs->entries = realloc(docs->entries, sizeof(DocEntry) * docs->capacity);
    }
    
    DocEntry* entry = &docs->entries[docs->count++];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    
    strncpy(entry->description, description, sizeof(entry->description) - 1);
    entry->description[sizeof(entry->description) - 1] = '\0';
    
    strncpy(entry->signature, signature, sizeof(entry->signature) - 1);
    entry->signature[sizeof(entry->signature) - 1] = '\0';
    
    strncpy(entry->example, example, sizeof(entry->example) - 1);
    entry->example[sizeof(entry->example) - 1] = '\0';
    
    strncpy(entry->file_path, file_path, sizeof(entry->file_path) - 1);
    entry->file_path[sizeof(entry->file_path) - 1] = '\0';
    
    entry->line_number = line;
}

void scan_goo_file(const char* filepath, Documentation* docs) {
    FILE* file = fopen(filepath, "r");
    if (!file) return;
    
    char line[1024];
    int line_number = 0;
    char current_function[256] = "";
    char current_description[1024] = "";
    
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        
        // Look for function documentation comments
        if (strstr(line, "// ") == line) {
            strncpy(current_description, line + 3, sizeof(current_description) - 1);
            // Remove newline
            current_description[strcspn(current_description, "\n")] = '\0';
        }
        
        // Look for function definitions
        if (strstr(line, "func ")) {
            char* func_start = strstr(line, "func ");
            if (func_start) {
                sscanf(func_start, "func %255s", current_function);
                
                // Remove parentheses and beyond for function name
                char* paren = strchr(current_function, '(');
                if (paren) *paren = '\0';
                
                if (strlen(current_function) > 0 && strlen(current_description) > 0) {
                    add_doc_entry(docs, current_function, current_description, 
                                line, "", filepath, line_number);
                    current_description[0] = '\0';
                }
            }
        }
        
        // Look for struct definitions
        if (strstr(line, "type ") && strstr(line, "struct")) {
            char struct_name[256];
            if (sscanf(line, "type %255s struct", struct_name) == 1) {
                if (strlen(current_description) > 0) {
                    add_doc_entry(docs, struct_name, current_description, 
                                line, "", filepath, line_number);
                    current_description[0] = '\0';
                }
            }
        }
    }
    
    fclose(file);
}

void scan_directory(const char* dir_path, Documentation* docs) {
    DIR* dir = opendir(dir_path);
    if (!dir) return;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        
        struct stat path_stat;
        stat(full_path, &path_stat);
        
        if (S_ISDIR(path_stat.st_mode)) {
            scan_directory(full_path, docs);
        } else if (strstr(entry->d_name, ".goo")) {
            printf("📄 Scanning: %s\n", full_path);
            scan_goo_file(full_path, docs);
        }
    }
    
    closedir(dir);
}

void generate_html_docs(const Documentation* docs, const char* output_dir) {
    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/index.html", output_dir);
    
    FILE* index_file = fopen(index_path, "w");
    if (!index_file) {
        printf("❌ Failed to create documentation index\n");
        return;
    }
    
    // HTML header
    fprintf(index_file, "<!DOCTYPE html>\n<html>\n<head>\n");
    fprintf(index_file, "<title>Goo Project Documentation</title>\n");
    fprintf(index_file, "<style>\n");
    fprintf(index_file, "body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 40px; }\n");
    fprintf(index_file, ".function { padding: 20px; border: 1px solid #e2e8f0; margin: 15px 0; border-radius: 8px; }\n");
    fprintf(index_file, ".signature { background: #f7fafc; padding: 10px; border-radius: 4px; font-family: monospace; }\n");
    fprintf(index_file, ".description { margin: 10px 0; color: #4a5568; }\n");
    fprintf(index_file, ".source-link { font-size: 12px; color: #718096; }\n");
    fprintf(index_file, "h1 { color: #2d3748; }\n");
    fprintf(index_file, "h2 { color: #4a5568; border-bottom: 2px solid #e2e8f0; padding-bottom: 5px; }\n");
    fprintf(index_file, "</style>\n");
    fprintf(index_file, "</head>\n<body>\n");
    
    fprintf(index_file, "<h1>🚀 Goo Project Documentation</h1>\n");
    fprintf(index_file, "<p>Generated on: %s</p>\n", __DATE__);
    
    // Group by type
    fprintf(index_file, "<h2>📋 Functions</h2>\n");
    
    for (int i = 0; i < docs->count; i++) {
        const DocEntry* entry = &docs->entries[i];
        
        fprintf(index_file, "<div class='function'>\n");
        fprintf(index_file, "<h3>%s</h3>\n", entry->name);
        fprintf(index_file, "<div class='signature'>%s</div>\n", entry->signature);
        fprintf(index_file, "<div class='description'>%s</div>\n", entry->description);
        fprintf(index_file, "<div class='source-link'>📍 <em>%s:%d</em></div>\n", 
                entry->file_path, entry->line_number);
        fprintf(index_file, "</div>\n");
    }
    
    fprintf(index_file, "</body>\n</html>\n");
    fclose(index_file);
    
    printf("📚 Documentation generated: %s\n", index_path);
}

int main(int argc, char* argv[]) {
    printf("📚 Goo Documentation Generator\n");
    printf("==============================\n\n");
    
    const char* source_dir = ".";
    const char* output_dir = "docs/generated";
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--source=", 9) == 0) {
            source_dir = argv[i] + 9;
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            output_dir = argv[i] + 9;
        }
    }
    
    printf("📁 Source directory: %s\n", source_dir);
    printf("📁 Output directory: %s\n\n", output_dir);
    
    // Create output directory
    mkdir(output_dir, 0755);
    
    Documentation docs;
    init_documentation(&docs);
    
    printf("🔍 Scanning for Goo files...\n");
    scan_directory(source_dir, &docs);
    
    printf("\n📊 Found %d documented items\n", docs.count);
    
    if (docs.count > 0) {
        generate_html_docs(&docs, output_dir);
        printf("\n🎉 Documentation generation complete!\n");
        printf("🌐 Open %s/index.html to view documentation\n", output_dir);
    } else {
        printf("\n💡 Tips for better documentation:\n");
        printf("  • Add comments above functions: // Description here\n");
        printf("  • Document struct types with comments\n");
        printf("  • Use descriptive function and variable names\n");
    }
    
    free(docs.entries);
    return 0;
}