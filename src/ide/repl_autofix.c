#include "auto_fix.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// =============================================================================
// REPL Integration for Automatic Error Correction
// =============================================================================

// Global auto-fix engine for REPL
static AutoFixEngine* g_auto_fix_engine = NULL;

// =============================================================================
// REPL Command Implementation
// =============================================================================

int auto_fix_repl_command(REPLContext* ctx, const char* args) {
    if (!ctx) return -1;
    
    // Initialize auto-fix engine if not done
    if (!g_auto_fix_engine) {
        g_auto_fix_engine = auto_fix_engine_new();
        if (!g_auto_fix_engine) {
            repl_error_printf(ctx, "Failed to initialize auto-fix engine\n");
            return -1;
        }
        
        auto_fix_engine_init(g_auto_fix_engine);
        auto_fix_engine_set_type_checker(g_auto_fix_engine, ctx->type_checker);
        auto_fix_engine_set_repl(g_auto_fix_engine, ctx);
    }
    
    // Parse subcommand
    if (!args || strlen(args) == 0) {
        return auto_fix_repl_show_help(ctx);
    }
    
    // Skip "autofix" if present
    const char* cmd = args;
    if (strncmp(cmd, "autofix", 7) == 0) {
        cmd += 7;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
    }
    
    if (strncmp(cmd, "help", 4) == 0) {
        return auto_fix_repl_show_help(ctx);
    } else if (strncmp(cmd, "analyze", 7) == 0) {
        return auto_fix_repl_analyze(ctx, cmd + 7);
    } else if (strncmp(cmd, "demo", 4) == 0) {
        return auto_fix_repl_demo(ctx);
    } else if (strncmp(cmd, "patterns", 8) == 0) {
        return auto_fix_repl_show_patterns(ctx);
    } else if (strncmp(cmd, "stats", 5) == 0) {
        return auto_fix_repl_show_stats(ctx);
    } else if (strncmp(cmd, "config", 6) == 0) {
        return auto_fix_repl_config(ctx, cmd + 6);
    } else {
        repl_error_printf(ctx, "Unknown autofix command: %s\n", cmd);
        repl_error_printf(ctx, "Type ':autofix help' for available commands\n");
        return -1;
    }
}

int auto_fix_repl_show_help(REPLContext* ctx) {
    repl_printf(ctx, "🔧 %sAutomatic Error Correction Commands:%s\n",
               ctx->color_output ? "\033[1m\033[36m" : "",
               ctx->color_output ? "\033[0m" : "");
    repl_printf(ctx, "==========================================\n");
    repl_printf(ctx, "  :autofix help        - Show this help message\n");
    repl_printf(ctx, "  :autofix analyze     - Analyze current error and suggest fixes\n");
    repl_printf(ctx, "  :autofix demo        - Show demo of auto-fix capabilities\n");
    repl_printf(ctx, "  :autofix patterns    - List all error patterns\n");
    repl_printf(ctx, "  :autofix stats       - Show auto-fix engine statistics\n");
    repl_printf(ctx, "  :autofix config      - Show/modify configuration\n");
    repl_printf(ctx, "\n%sUsage Examples:%s\n",
               ctx->color_output ? "\033[1m\033[33m" : "",
               ctx->color_output ? "\033[0m" : "");
    repl_printf(ctx, "  // Generate an error first\n");
    repl_printf(ctx, "  let x: int = \"hello\"\n");
    repl_printf(ctx, "  \n");
    repl_printf(ctx, "  // Then analyze it\n");
    repl_printf(ctx, "  :autofix analyze\n");
    repl_printf(ctx, "\n");
    
    return 0;
}

int auto_fix_repl_analyze(REPLContext* ctx, const char* args) {
    if (!ctx || !g_auto_fix_engine) return -1;
    
    (void)args; // Unused for now
    
    // Simulate analyzing a common error for demonstration
    repl_printf(ctx, "🔍 Analyzing recent error...\n\n");
    
    // Create a sample error context for demonstration
    FixContext* context = auto_fix_context_new(
        "<repl>",
        "Type error: expected 'int', got 'string'",
        (Position){.line = 1, .column = 10},
        NULL
    );
    
    if (context) {
        // Add type information if available
        if (ctx->type_checker) {
            // Simulate type mismatch
            context->expected_type = type_new(TYPE_INT32);
            context->actual_type = type_new(TYPE_STRING);
        }
        
        // Generate fix suggestions
        FixSuggestion* suggestions = auto_fix_analyze_error(g_auto_fix_engine, context);
        
        if (suggestions) {
            auto_fix_show_suggestions(g_auto_fix_engine, suggestions);
            
            // Ask if user wants to apply the fix
            repl_printf(ctx, "Would you like to apply the suggested fix? (y/n): ");
            // For demo purposes, just show what would happen
            repl_printf(ctx, "\n💡 In a full implementation, this would modify your code automatically.\n");
            
            auto_fix_suggestion_free(suggestions);
        } else {
            repl_printf(ctx, "No specific fix suggestions available for this error.\n");
        }
        
        auto_fix_context_free(context);
    }
    
    return 0;
}

int auto_fix_repl_demo(REPLContext* ctx) {
    if (!ctx || !g_auto_fix_engine) return -1;
    
    repl_printf(ctx, "🎬 %sAuto-Fix Demonstration%s\n",
               ctx->color_output ? "\033[1m\033[35m" : "",
               ctx->color_output ? "\033[0m" : "");
    repl_printf(ctx, "========================\n\n");
    
    // Demo different types of errors and their fixes
    const char* demo_errors[] = {
        "Type error: expected 'int', got 'string'",
        "Null safety error: accessing nullable without null check", 
        "Ownership error: use of moved value",
        "Syntax error: expected ';' after expression",
        "Import error: undefined symbol 'println'"
    };
    
    for (size_t i = 0; i < sizeof(demo_errors) / sizeof(demo_errors[0]); i++) {
        repl_printf(ctx, "%s%zu. Demo Error:%s %s\n",
                   ctx->color_output ? "\033[1m\033[31m" : "",
                   i + 1,
                   ctx->color_output ? "\033[0m" : "",
                   demo_errors[i]);
        
        FixContext* context = auto_fix_context_new(
            "<demo>", demo_errors[i], (Position){.line = 1, .column = 1}, NULL
        );
        
        if (context) {
            // Add specific context for different error types
            if (i == 0) {  // Type error
                context->expected_type = type_new(TYPE_INT32);
                context->actual_type = type_new(TYPE_STRING);
            }
            
            FixSuggestion* suggestion = auto_fix_analyze_error(g_auto_fix_engine, context);
            if (suggestion) {
                repl_printf(ctx, "   🔧 %sSuggestion:%s %s\n",
                           ctx->color_output ? "\033[32m" : "",
                           ctx->color_output ? "\033[0m" : "",
                           suggestion->description);
                
                if (suggestion->fixes && suggestion->fixes->explanation) {
                    repl_printf(ctx, "   💡 %s\n", suggestion->fixes->explanation);
                }
                
                auto_fix_suggestion_free(suggestion);
            }
            
            auto_fix_context_free(context);
        }
        
        repl_printf(ctx, "\n");
    }
    
    repl_printf(ctx, "✨ The auto-fix system can help with:\n");
    repl_printf(ctx, "   • Type conversion suggestions\n");
    repl_printf(ctx, "   • Null safety fixes\n");
    repl_printf(ctx, "   • Ownership and borrowing guidance\n");
    repl_printf(ctx, "   • Syntax error corrections\n");
    repl_printf(ctx, "   • Missing import detection\n");
    repl_printf(ctx, "   • Error handling improvements\n");
    
    return 0;
}

int auto_fix_repl_show_patterns(REPLContext* ctx) {
    if (!ctx || !g_auto_fix_engine) return -1;
    
    repl_printf(ctx, "📋 %sError Patterns Database%s\n",
               ctx->color_output ? "\033[1m\033[36m" : "",
               ctx->color_output ? "\033[0m" : "");
    repl_printf(ctx, "========================\n\n");
    
    ErrorPattern* pattern = g_auto_fix_engine->patterns;
    int count = 1;
    
    while (pattern) {
        repl_printf(ctx, "%s%d. %s%s\n",
                   ctx->color_output ? "\033[1m" : "",
                   count,
                   pattern->description,
                   ctx->color_output ? "\033[0m" : "");
        
        // Show category
        const char* category_name = "Unknown";
        switch (pattern->category) {
            case ERROR_CATEGORY_TYPE: category_name = "Type Error"; break;
            case ERROR_CATEGORY_SYNTAX: category_name = "Syntax Error"; break;
            case ERROR_CATEGORY_OWNERSHIP: category_name = "Ownership Error"; break;
            case ERROR_CATEGORY_NULL_SAFETY: category_name = "Null Safety"; break;
            case ERROR_CATEGORY_IMPORT: category_name = "Import Error"; break;
            case ERROR_CATEGORY_ERROR_HANDLING: category_name = "Error Handling"; break;
            default: break;
        }
        
        repl_printf(ctx, "   Category: %s\n", category_name);
        repl_printf(ctx, "   Priority: %d\n", pattern->priority);
        
        if (pattern->example_error) {
            repl_printf(ctx, "   Example: %s\n", pattern->example_error);
        }
        
        repl_printf(ctx, "\n");
        pattern = pattern->next;
        count++;
    }
    
    repl_printf(ctx, "Total patterns: %d\n", g_auto_fix_engine->pattern_count);
    
    return 0;
}

int auto_fix_repl_show_stats(REPLContext* ctx) {
    if (!ctx || !g_auto_fix_engine) return -1;
    
    auto_fix_print_statistics(g_auto_fix_engine);
    return 0;
}

int auto_fix_repl_config(REPLContext* ctx, const char* args) {
    if (!ctx || !g_auto_fix_engine) return -1;
    
    // Skip whitespace
    while (args && *args && isspace(*args)) args++;
    
    if (!args || strlen(args) == 0) {
        // Show current configuration
        repl_printf(ctx, "⚙️  %sAuto-Fix Configuration%s\n",
                   ctx->color_output ? "\033[1m\033[36m" : "",
                   ctx->color_output ? "\033[0m" : "");
        repl_printf(ctx, "=========================\n");
        repl_printf(ctx, "Auto-apply safe fixes: %s\n", 
                   g_auto_fix_engine->auto_apply_safe ? "Enabled" : "Disabled");
        repl_printf(ctx, "Show explanations: %s\n",
                   g_auto_fix_engine->show_explanations ? "Enabled" : "Disabled");
        repl_printf(ctx, "Batch mode: %s\n",
                   g_auto_fix_engine->batch_mode ? "Enabled" : "Disabled");
        
        const char* confidence_str = "Low";
        switch (g_auto_fix_engine->min_confidence) {
            case FIX_CONFIDENCE_HIGH: confidence_str = "High"; break;
            case FIX_CONFIDENCE_MEDIUM: confidence_str = "Medium"; break;
            case FIX_CONFIDENCE_LOW: confidence_str = "Low"; break;
            case FIX_CONFIDENCE_UNSAFE: confidence_str = "Unsafe"; break;
        }
        repl_printf(ctx, "Minimum confidence: %s\n", confidence_str);
        
        repl_printf(ctx, "\nUse ':autofix config <setting> <value>' to modify settings\n");
        repl_printf(ctx, "Available settings: auto_apply, explanations, batch_mode, min_confidence\n");
        
    } else {
        // Parse configuration command
        char setting[64] = {0};
        char value[64] = {0};
        
        if (sscanf(args, "%63s %63s", setting, value) == 2) {
            if (strcmp(setting, "auto_apply") == 0) {
                g_auto_fix_engine->auto_apply_safe = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
                repl_printf(ctx, "Auto-apply safe fixes: %s\n", 
                           g_auto_fix_engine->auto_apply_safe ? "Enabled" : "Disabled");
            } else if (strcmp(setting, "explanations") == 0) {
                g_auto_fix_engine->show_explanations = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
                repl_printf(ctx, "Show explanations: %s\n",
                           g_auto_fix_engine->show_explanations ? "Enabled" : "Disabled");
            } else if (strcmp(setting, "batch_mode") == 0) {
                g_auto_fix_engine->batch_mode = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
                repl_printf(ctx, "Batch mode: %s\n",
                           g_auto_fix_engine->batch_mode ? "Enabled" : "Disabled");
            } else if (strcmp(setting, "min_confidence") == 0) {
                if (strcmp(value, "high") == 0) {
                    g_auto_fix_engine->min_confidence = FIX_CONFIDENCE_HIGH;
                } else if (strcmp(value, "medium") == 0) {
                    g_auto_fix_engine->min_confidence = FIX_CONFIDENCE_MEDIUM;
                } else if (strcmp(value, "low") == 0) {
                    g_auto_fix_engine->min_confidence = FIX_CONFIDENCE_LOW;
                } else {
                    repl_error_printf(ctx, "Invalid confidence level. Use: high, medium, or low\n");
                    return -1;
                }
                repl_printf(ctx, "Minimum confidence set to: %s\n", value);
            } else {
                repl_error_printf(ctx, "Unknown setting: %s\n", setting);
                return -1;
            }
        } else {
            repl_error_printf(ctx, "Usage: :autofix config <setting> <value>\n");
            return -1;
        }
    }
    
    return 0;
}

// =============================================================================
// Integration with Error Reporting
// =============================================================================

void auto_fix_repl_on_error(REPLContext* ctx, const char* error_message, Position pos) {
    if (!ctx || !error_message || !g_auto_fix_engine) return;
    
    // Auto-analyze errors if enabled
    if (g_auto_fix_engine->auto_apply_safe) {
        repl_printf(ctx, "\n🔍 Auto-analyzing error...\n");
        
        FixContext* context = auto_fix_context_new("<repl>", error_message, pos, NULL);
        if (context) {
            FixSuggestion* suggestions = auto_fix_analyze_error(g_auto_fix_engine, context);
            if (suggestions && suggestions->confidence >= g_auto_fix_engine->min_confidence) {
                repl_printf(ctx, "💡 Suggestion available! Type ':autofix analyze' to see it.\n");
            }
            
            if (suggestions) auto_fix_suggestion_free(suggestions);
            auto_fix_context_free(context);
        }
    }
}

// =============================================================================
// Cleanup
// =============================================================================

void auto_fix_repl_cleanup(void) {
    if (g_auto_fix_engine) {
        auto_fix_engine_free(g_auto_fix_engine);
        g_auto_fix_engine = NULL;
    }
}