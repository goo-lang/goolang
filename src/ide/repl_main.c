#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
    printf("  -e, --eval EXPR    Evaluate expression and exit\n");
    printf("  --no-color         Disable color output\n");
    printf("  --no-types         Disable type information display\n");
    printf("  --no-timing        Disable execution timing\n");
    printf("  --history-size N   Set history size (default: 1000)\n");
}

int main(int argc, char** argv) {
    REPLContext* ctx = repl_context_new();
    if (!ctx) {
        fprintf(stderr, "Failed to create REPL context\n");
        return 1;
    }
    
    // Parse command line arguments
    char* eval_expr = NULL;
    bool show_help = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            show_help = true;
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--eval") == 0) {
            if (i + 1 < argc) {
                eval_expr = argv[++i];
            } else {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i]);
                repl_context_free(ctx);
                return 1;
            }
        } else if (strcmp(argv[i], "--no-color") == 0) {
            ctx->color_output = false;
        } else if (strcmp(argv[i], "--no-types") == 0) {
            ctx->show_types = false;
        } else if (strcmp(argv[i], "--no-timing") == 0) {
            ctx->show_timing = false;
        } else if (strcmp(argv[i], "--history-size") == 0) {
            if (i + 1 < argc) {
                int size = atoi(argv[++i]);
                if (size > 0 && size <= 10000) {
                    ctx->max_history = size;
                } else {
                    fprintf(stderr, "Error: Invalid history size %d\n", size);
                    repl_context_free(ctx);
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: --history-size requires an argument\n");
                repl_context_free(ctx);
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unknown argument %s\n", argv[i]);
            print_usage(argv[0]);
            repl_context_free(ctx);
            return 1;
        }
    }
    
    if (show_help) {
        print_usage(argv[0]);
        repl_context_free(ctx);
        return 0;
    }
    
    // Initialize REPL
    if (repl_init(ctx) != 0) {
        fprintf(stderr, "Failed to initialize REPL\n");
        repl_context_free(ctx);
        return 1;
    }
    
    int result = 0;
    
    if (eval_expr) {
        // Evaluate single expression and exit
        result = repl_eval_string(ctx, eval_expr);
    } else {
        // Run interactive REPL
        result = repl_run(ctx);
    }
    
    // Cleanup
    repl_cleanup(ctx);
    repl_context_free(ctx);
    
    return result;
}