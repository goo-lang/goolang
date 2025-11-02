#include "repl.h"
#include "repl_type_info.h"
#include <stdio.h>
#include <stdlib.h>

// =============================================================================
// Enhanced REPL with Rich Type Information - Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    printf("🚀 Goo Enhanced REPL with Rich Type Information\n");
    printf("============================================\n");
    printf("Features:\n");
    printf("• Interactive type exploration with :inspect command\n");
    printf("• Live type information during evaluation\n");
    printf("• Detailed type analysis with examples and performance characteristics\n");
    printf("• Method and property suggestions\n");
    printf("• Memory safety and ownership information\n");
    printf("• Error union (!T) and nullable type (?T) support\n");
    printf("\n");
    
    // Create and initialize REPL context
    REPLContext* ctx = repl_context_new();
    if (!ctx) {
        fprintf(stderr, "Failed to create REPL context\n");
        return 1;
    }
    
    // Initialize the REPL
    if (repl_init(ctx) != 0) {
        fprintf(stderr, "Failed to initialize REPL\n");
        repl_context_free(ctx);
        return 1;
    }
    
    // Show enhanced features in the banner
    repl_printf(ctx, "%sEnhanced Commands:%s\n",
               ctx->color_output ? "\033[1m\033[36m" : "",
               ctx->color_output ? "\033[0m" : "");
    repl_printf(ctx, "  :inspect <type>  - Comprehensive type analysis\n");
    repl_printf(ctx, "  :type <expr>     - Quick type information\n");
    repl_printf(ctx, "\n%sExample Usage:%s\n",
               ctx->color_output ? "\033[1m\033[33m" : "",
               ctx->color_output ? "\033[0m" : "");
    repl_printf(ctx, "  :inspect int     - Learn about integer types\n");
    repl_printf(ctx, "  :inspect !int    - Explore error unions\n");
    repl_printf(ctx, "  :inspect ?string - Understand nullable types\n");
    repl_printf(ctx, "  :inspect []int   - Array and slice information\n");
    repl_printf(ctx, "\n");
    
    // Run the interactive REPL
    int result = repl_run_interactive(ctx);
    
    // Cleanup
    repl_cleanup(ctx);
    repl_context_free(ctx);
    
    return result;
}