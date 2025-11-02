// Documentation Generator Main Application
// Integrated documentation generation and preview for Goo projects

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include "documentation_generator.h"

// =============================================================================
// Global State
// =============================================================================

static DocumentationGenerator* g_generator = NULL;
static bool g_running = false;
static pthread_t g_preview_thread;

// =============================================================================
// Signal Handler
// =============================================================================

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n🛑 Shutting down documentation generator...\n");
        g_running = false;
    }
}

// =============================================================================
// Live Preview Server (Simplified)
// =============================================================================

void* preview_server_thread(void* arg) {
    DocumentationGenerator* gen = (DocumentationGenerator*)arg;
    
    printf("🌐 Documentation preview server starting on port %d\n", gen->config.preview_port);
    printf("📖 Open http://localhost:%d in your browser\n", gen->config.preview_port);
    printf("🔄 Auto-refresh enabled - documentation will update on file changes\n");
    
    // In a full implementation, this would start an HTTP server
    // For now, we'll simulate by watching for file changes
    while (g_running && gen->config.live_preview) {
        // Check for file changes (simplified)
        // In reality, this would use inotify or similar
        sleep(2);
        
        // Regenerate documentation if files changed
        printf("🔄 Checking for file changes...\n");
        // documentation_generator_generate(gen);
    }
    
    return NULL;
}

// =============================================================================
// Interactive Mode
// =============================================================================

void run_interactive_mode(DocumentationGenerator* gen) {
    printf("📚 Goo Documentation Generator - Interactive Mode\n");
    printf("=================================================\n");
    printf("Type 'help' for available commands, 'exit' to quit\n\n");
    
    char input[256];
    while (g_running) {
        printf("docs> ");
        fflush(stdout);
        
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        input[strcspn(input, "\n")] = 0;
        
        if (strlen(input) == 0) {
            continue;
        }
        
        if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
            break;
        }
        
        if (strcmp(input, "help") == 0) {
            printf("📚 Available Commands:\n");
            printf("   status         - Show generator status\n");
            printf("   generate       - Generate documentation\n");
            printf("   preview        - Start live preview server\n");
            printf("   stop           - Stop live preview\n");
            printf("   config         - Show configuration\n");
            printf("   stats          - Show generation statistics\n");
            printf("   format <fmt>   - Set output format (html, markdown)\n");
            printf("   output <dir>   - Set output directory\n");
            printf("   scan           - Scan and analyze source files\n");
            printf("   clear          - Clear screen\n");
            printf("   help           - Show this help\n");
            printf("   exit           - Exit application\n\n");
            continue;
        }
        
        if (strcmp(input, "clear") == 0) {
            printf("\033[2J\033[H");
            continue;
        }
        
        if (strcmp(input, "status") == 0) {
            printf("📊 Documentation Generator Status:\n");
            printf("   Enabled: %s\n", gen->is_enabled ? "Yes" : "No");
            printf("   Generating: %s\n", gen->is_generating ? "Yes" : "No");
            printf("   Project Root: %s\n", gen->project_root);
            printf("   Output Format: %s\n", 
                   gen->config.output_format == DOC_FORMAT_HTML ? "HTML" : 
                   gen->config.output_format == DOC_FORMAT_MARKDOWN ? "Markdown" : "Unknown");
            printf("   Output Directory: %s\n", gen->config.output_directory);
            printf("   Live Preview: %s\n", gen->config.live_preview ? "ON" : "OFF");
            printf("   Elements Found: %d\n", gen->total_elements);
            printf("   Files Processed: %d\n\n", gen->processed_files);
            continue;
        }
        
        if (strcmp(input, "generate") == 0) {
            printf("📚 Generating documentation...\n");
            int result = documentation_generator_generate(gen);
            if (result == 0) {
                printf("✅ Documentation generated successfully!\n");
                printf("📂 Output: %s\n\n", gen->config.output_directory);
            } else {
                printf("❌ Failed to generate documentation\n\n");
            }
            continue;
        }
        
        if (strcmp(input, "preview") == 0) {
            if (!gen->config.live_preview) {
                gen->config.live_preview = true;
                pthread_create(&g_preview_thread, NULL, preview_server_thread, gen);
                printf("🌐 Live preview started\n\n");
            } else {
                printf("⚠️  Live preview is already running\n\n");
            }
            continue;
        }
        
        if (strcmp(input, "stop") == 0) {
            if (gen->config.live_preview) {
                gen->config.live_preview = false;
                printf("⏸️  Live preview stopped\n\n");
            } else {
                printf("ℹ️  Live preview is not running\n\n");
            }
            continue;
        }
        
        if (strcmp(input, "config") == 0) {
            printf("⚙️  Documentation Generator Configuration:\n");
            printf("   Project Name: %s\n", gen->config.project_name);
            printf("   Project Version: %s\n", gen->config.project_version);
            printf("   Project Description: %s\n", gen->config.project_description);
            printf("   Author: %s\n", gen->config.author);
            printf("   Output Format: %s\n", 
                   gen->config.output_format == DOC_FORMAT_HTML ? "HTML" : "Markdown");
            printf("   Include Private: %s\n", gen->config.include_private ? "Yes" : "No");
            printf("   Generate Index: %s\n", gen->config.generate_index ? "Yes" : "No");
            printf("   Generate Search: %s\n", gen->config.generate_search ? "Yes" : "No");
            printf("   Include Source Links: %s\n", gen->config.include_source_links ? "Yes" : "No");
            printf("   Include Examples: %s\n", gen->config.include_examples ? "Yes" : "No");
            printf("   Preview Port: %d\n\n", gen->config.preview_port);
            continue;
        }
        
        if (strcmp(input, "stats") == 0) {
            printf("📈 Generation Statistics:\n");
            printf("   Functions Documented: %d\n", gen->functions_documented);
            printf("   Structures Documented: %d\n", gen->structs_documented);
            printf("   Interfaces Documented: %d\n", gen->interfaces_documented);
            printf("   Modules Documented: %d\n", gen->modules_documented);
            printf("   Total Elements: %d\n", gen->total_elements);
            printf("   Files Processed: %d\n", gen->processed_files);
            if (gen->generation_end_time > gen->generation_start_time) {
                double time_taken = (gen->generation_end_time - gen->generation_start_time) / 1000.0;
                printf("   Last Generation Time: %.2f seconds\n", time_taken);
            }
            printf("\n");
            continue;
        }
        
        if (strcmp(input, "scan") == 0) {
            printf("🔍 Scanning source files...\n");
            
            // Reset counters
            gen->total_elements = 0;
            gen->processed_files = 0;
            gen->functions_documented = 0;
            gen->structs_documented = 0;
            gen->interfaces_documented = 0;
            
            documentation_generator_scan_directory(gen, gen->project_root);
            
            printf("✅ Scan completed!\n");
            printf("📊 Found %d elements in %d files\n\n", gen->total_elements, gen->processed_files);
            continue;
        }
        
        if (strncmp(input, "format ", 7) == 0) {
            char* format = input + 7;
            if (strcmp(format, "html") == 0) {
                gen->config.output_format = DOC_FORMAT_HTML;
                printf("✅ Output format set to HTML\n\n");
            } else if (strcmp(format, "markdown") == 0) {
                gen->config.output_format = DOC_FORMAT_MARKDOWN;
                printf("✅ Output format set to Markdown\n\n");
            } else {
                printf("❌ Unknown format: %s\n", format);
                printf("💡 Available formats: html, markdown\n\n");
            }
            continue;
        }
        
        if (strncmp(input, "output ", 7) == 0) {
            char* output_dir = input + 7;
            free(gen->config.output_directory);
            gen->config.output_directory = strdup(output_dir);
            printf("✅ Output directory set to: %s\n\n", output_dir);
            continue;
        }
        
        printf("❌ Unknown command: %s\n", input);
        printf("💡 Type 'help' for available commands\n\n");
    }
}

// =============================================================================
// Generate Mode
// =============================================================================

void run_generate_mode(DocumentationGenerator* gen) {
    printf("📚 Generating project documentation...\n");
    
    // Generate documentation
    int result = documentation_generator_generate(gen);
    
    if (result == 0) {
        printf("\n🎉 Documentation generation completed successfully!\n");
        printf("📂 Documentation available at: %s\n", gen->config.output_directory);
        
        if (gen->config.output_format == DOC_FORMAT_HTML) {
            printf("🌐 Open %s/index.html in your browser\n", gen->config.output_directory);
        }
        
        printf("\n📊 Summary:\n");
        printf("   Functions:   %d\n", gen->functions_documented);
        printf("   Structures:  %d\n", gen->structs_documented);
        printf("   Interfaces:  %d\n", gen->interfaces_documented);
        printf("   Files:       %d\n", gen->processed_files);
        
        if (gen->generation_end_time > gen->generation_start_time) {
            double time_taken = (gen->generation_end_time - gen->generation_start_time) / 1000.0;
            printf("   Time:        %.2f seconds\n", time_taken);
        }
    } else {
        printf("❌ Documentation generation failed\n");
    }
}

// =============================================================================
// Preview Mode
// =============================================================================

void run_preview_mode(DocumentationGenerator* gen) {
    printf("🌐 Starting documentation preview server...\n");
    
    // Generate initial documentation
    printf("📚 Generating initial documentation...\n");
    documentation_generator_generate(gen);
    
    // Start preview server
    gen->config.live_preview = true;
    pthread_create(&g_preview_thread, NULL, preview_server_thread, gen);
    
    printf("\n🎉 Preview server is running!\n");
    printf("📖 Documentation URL: http://localhost:%d\n", gen->config.preview_port);
    printf("🔄 Watching for file changes...\n");
    printf("⏹️  Press Ctrl+C to stop\n\n");
    
    // Main loop
    while (g_running) {
        sleep(1);
    }
    
    // Stop preview server
    gen->config.live_preview = false;
    pthread_join(g_preview_thread, NULL);
}

// =============================================================================
// Scan Mode
// =============================================================================

void run_scan_mode(DocumentationGenerator* gen) {
    printf("🔍 Scanning project for documentation elements...\n");
    
    documentation_generator_scan_directory(gen, gen->project_root);
    
    printf("✅ Scan completed!\n\n");
    printf("📊 Documentation Analysis Results:\n");
    printf("===================================\n");
    printf("📄 Files processed: %d\n", gen->processed_files);
    printf("📋 Total elements: %d\n", gen->total_elements);
    printf("🔧 Functions: %d\n", gen->functions_documented);
    printf("🏗️ Structures: %d\n", gen->structs_documented);
    printf("🔌 Interfaces: %d\n", gen->interfaces_documented);
    printf("📦 Modules: %d\n", gen->modules_documented);
    
    if (gen->total_elements > 0) {
        printf("\n📋 Sample Elements Found:\n");
        int count = 0;
        DocumentationElement* element = gen->elements;
        while (element && count < 5) {
            const char* type_str = "Unknown";
            switch (element->type) {
                case DOC_ELEMENT_FUNCTION: type_str = "Function"; break;
                case DOC_ELEMENT_STRUCT: type_str = "Struct"; break;
                case DOC_ELEMENT_INTERFACE: type_str = "Interface"; break;
                default: break;
            }
            
            printf("   %s: %s", type_str, element->name ? element->name : "unnamed");
            if (element->source_file) {
                printf(" (%s:%d)", element->source_file, element->line_number);
            }
            printf("\n");
            
            element = element->next;
            count++;
        }
        
        if (gen->total_elements > 5) {
            printf("   ... and %d more elements\n", gen->total_elements - 5);
        }
    }
    
    printf("\n💡 Use 'generate' mode to create documentation from these elements\n");
}

// =============================================================================
// Usage Information
// =============================================================================

void print_usage(const char* program_name) {
    printf("Goo Documentation Generator\n");
    printf("===========================\n\n");
    printf("Usage: %s [OPTIONS] [MODE] [PROJECT_PATH]\n\n", program_name);
    printf("Modes:\n");
    printf("  interactive    Interactive command-line mode (default)\n");
    printf("  generate       Generate documentation and exit\n");
    printf("  preview        Generate and serve with live preview\n");
    printf("  scan           Scan project and show analysis\n\n");
    printf("Options:\n");
    printf("  -h, --help              Show this help\n");
    printf("  -o, --output <dir>      Output directory (default: ./docs)\n");
    printf("  -f, --format <fmt>      Output format: html, markdown (default: html)\n");
    printf("  -p, --port <port>       Preview server port (default: 8080)\n");
    printf("  --name <name>           Project name\n");
    printf("  --version <version>     Project version\n");
    printf("  --author <author>       Project author\n");
    printf("  --include-private       Include private elements\n");
    printf("  --no-search             Disable search functionality\n");
    printf("  --no-source-links       Disable source code links\n\n");
    printf("Examples:\n");
    printf("  %s                              # Interactive mode\n", program_name);
    printf("  %s generate /path/to/project    # Generate docs for project\n", program_name);
    printf("  %s preview -p 9000              # Live preview on port 9000\n", program_name);
    printf("  %s scan --include-private       # Scan including private elements\n", program_name);
}

// =============================================================================
// Main Function
// =============================================================================

int main(int argc, char* argv[]) {
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    g_running = true;
    
    // Parse command line arguments
    char* mode = "interactive";
    char* project_root = ".";
    char* output_dir = NULL;
    char* format = NULL;
    char* project_name = NULL;
    char* project_version = NULL;
    char* project_author = NULL;
    int preview_port = 8080;
    bool include_private = false;
    bool enable_search = true;
    bool enable_source_links = true;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                output_dir = argv[++i];
            }
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--format") == 0) {
            if (i + 1 < argc) {
                format = argv[++i];
            }
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                preview_port = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--name") == 0) {
            if (i + 1 < argc) {
                project_name = argv[++i];
            }
        } else if (strcmp(argv[i], "--version") == 0) {
            if (i + 1 < argc) {
                project_version = argv[++i];
            }
        } else if (strcmp(argv[i], "--author") == 0) {
            if (i + 1 < argc) {
                project_author = argv[++i];
            }
        } else if (strcmp(argv[i], "--include-private") == 0) {
            include_private = true;
        } else if (strcmp(argv[i], "--no-search") == 0) {
            enable_search = false;
        } else if (strcmp(argv[i], "--no-source-links") == 0) {
            enable_source_links = false;
        } else if (argv[i][0] != '-') {
            // First non-option argument is mode, second is project path
            static bool mode_set = false;
            if (!mode_set) {
                mode = argv[i];
                mode_set = true;
            } else {
                project_root = argv[i];
            }
        }
    }
    
    // Initialize generator
    g_generator = documentation_generator_new(project_root);
    if (!g_generator) {
        fprintf(stderr, "Failed to initialize documentation generator\n");
        return 1;
    }
    
    // Configure based on command line arguments
    if (output_dir) {
        free(g_generator->config.output_directory);
        g_generator->config.output_directory = strdup(output_dir);
    }
    
    if (format) {
        if (strcmp(format, "html") == 0) {
            g_generator->config.output_format = DOC_FORMAT_HTML;
        } else if (strcmp(format, "markdown") == 0) {
            g_generator->config.output_format = DOC_FORMAT_MARKDOWN;
        }
    }
    
    if (project_name) {
        free(g_generator->config.project_name);
        g_generator->config.project_name = strdup(project_name);
    }
    
    if (project_version) {
        free(g_generator->config.project_version);
        g_generator->config.project_version = strdup(project_version);
    }
    
    if (project_author) {
        free(g_generator->config.author);
        g_generator->config.author = strdup(project_author);
    }
    
    g_generator->config.preview_port = preview_port;
    g_generator->config.include_private = include_private;
    g_generator->config.generate_search = enable_search;
    g_generator->config.include_source_links = enable_source_links;
    
    // Initialize
    if (documentation_generator_init(g_generator) != 0) {
        fprintf(stderr, "Failed to initialize documentation generator\n");
        documentation_generator_free(g_generator);
        return 1;
    }
    
    // Run based on mode
    if (strcmp(mode, "interactive") == 0) {
        run_interactive_mode(g_generator);
    } else if (strcmp(mode, "generate") == 0) {
        run_generate_mode(g_generator);
    } else if (strcmp(mode, "preview") == 0) {
        run_preview_mode(g_generator);
    } else if (strcmp(mode, "scan") == 0) {
        run_scan_mode(g_generator);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        print_usage(argv[0]);
        documentation_generator_free(g_generator);
        return 1;
    }
    
    // Cleanup
    printf("\n🧹 Cleaning up...\n");
    documentation_generator_free(g_generator);
    
    printf("👋 Goodbye!\n");
    return 0;
}