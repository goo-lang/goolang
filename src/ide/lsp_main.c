#include "lsp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

// Global logging state
bool lsp_logging_enabled = false;
FILE* lsp_log_file = NULL;

// Global server instance for signal handling
static LSPServer* g_server = NULL;

static void signal_handler(int sig) {
    (void)sig; // Unused parameter
    
    if (g_server) {
        lsp_log_info("Received signal, shutting down gracefully");
        g_server->running = false;
    }
}

static void setup_signal_handlers(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN); // Ignore broken pipe
}

static void print_usage(const char* program_name) {
    fprintf(stderr, "Usage: %s [options]\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -h, --help     Show this help message\n");
    fprintf(stderr, "  -v, --verbose  Enable verbose logging\n");
    fprintf(stderr, "  -l, --log FILE Log to specified file\n");
    fprintf(stderr, "  --stdio        Use stdio for communication (default)\n");
    fprintf(stderr, "  --socket PORT  Use TCP socket on specified port\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "The Goo Language Server implements the Language Server Protocol (LSP)\n");
    fprintf(stderr, "for providing IDE features like completion, hover, and diagnostics.\n");
}

static void enable_logging(const char* log_file) {
    lsp_logging_enabled = true;
    
    if (log_file) {
        lsp_log_file = fopen(log_file, "a");
        if (!lsp_log_file) {
            fprintf(stderr, "Warning: Could not open log file %s, using stderr\n", log_file);
            lsp_log_file = stderr;
        }
    } else {
        lsp_log_file = stderr;
    }
}

int main(int argc, char* argv[]) {
    bool verbose = false;
    char* log_file = NULL;
    bool use_stdio = true;
    int socket_port = 0;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            if (i + 1 < argc) {
                log_file = argv[++i];
            } else {
                fprintf(stderr, "Error: --log requires a filename\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--stdio") == 0) {
            use_stdio = true;
        } else if (strcmp(argv[i], "--socket") == 0) {
            if (i + 1 < argc) {
                socket_port = atoi(argv[++i]);
                use_stdio = false;
            } else {
                fprintf(stderr, "Error: --socket requires a port number\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Enable logging if requested
    if (verbose) {
        enable_logging(log_file);
    }
    
    // Setup signal handlers
    setup_signal_handlers();
    
    // Create and configure server
    LSPServer* server = lsp_server_new();
    if (!server) {
        fprintf(stderr, "Error: Failed to create LSP server\n");
        return 1;
    }
    
    g_server = server; // For signal handling
    
    // Configure communication method
    if (use_stdio) {
        server->input_stream = stdin;
        server->output_stream = stdout;
        lsp_log_info("Using stdio for communication");
    } else {
        // Socket communication would be implemented here
        fprintf(stderr, "Error: Socket communication not yet implemented\n");
        lsp_server_free(server);
        return 1;
    }
    
    // Set server capabilities
    LSPServerCapabilities capabilities = {
        .text_document_sync_full = true,
        .completion_provider = true,
        .hover_provider = true,
        .definition_provider = true,
        .references_provider = true,
        .document_symbol_provider = true,
        .workspace_symbol_provider = false, // Not implemented yet
        .code_action_provider = false,      // Not implemented yet
        .document_formatting_provider = false, // Not implemented yet
        .rename_provider = false            // Not implemented yet
    };
    
    // Set completion trigger characters
    capabilities.completion_trigger_characters[0] = ".";
    capabilities.completion_trigger_count = 1;
    
    lsp_server_set_capabilities(server, &capabilities);
    
    lsp_log_info("Goo Language Server starting...");
    
    // Run the server
    int result = lsp_server_run(server);
    
    // Cleanup
    lsp_server_free(server);
    g_server = NULL;
    
    lsp_log_info("Goo Language Server stopped");
    
    // Close log file if opened
    if (lsp_log_file && lsp_log_file != stderr) {
        fclose(lsp_log_file);
    }
    
    return result;
}