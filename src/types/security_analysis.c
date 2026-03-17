#include "security.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Taint Info Operations
// =============================================================================

TaintInfo taint_info_none(void) {
    return (TaintInfo){
        .sources = TAINT_SOURCE_NONE,
        .is_tainted = false,
        .is_sanitized = false,
        .sanitizer_name = NULL,
        .taint_origin = {0},
        .sanitize_point = {0},
    };
}

TaintInfo taint_info_tainted(TaintSource source, Position origin) {
    return (TaintInfo){
        .sources = (uint32_t)source,
        .is_tainted = true,
        .is_sanitized = false,
        .sanitizer_name = NULL,
        .taint_origin = origin,
        .sanitize_point = {0},
    };
}

TaintInfo taint_info_sanitized(TaintInfo original, const char* sanitizer, Position point) {
    return (TaintInfo){
        .sources = original.sources,
        .is_tainted = false,
        .is_sanitized = true,
        .sanitizer_name = sanitizer,
        .taint_origin = original.taint_origin,
        .sanitize_point = point,
    };
}

bool taint_info_is_clean(const TaintInfo* info) {
    if (!info) return true;
    return !info->is_tainted;
}

// =============================================================================
// Taint Propagation Rules
// =============================================================================

TaintInfo taint_propagate_binary(TaintInfo left, TaintInfo right) {
    // If either operand is tainted, result is tainted (union of sources)
    if (!left.is_tainted && !right.is_tainted) {
        return taint_info_none();
    }

    TaintInfo result = {
        .sources = left.sources | right.sources,
        .is_tainted = true,
        .is_sanitized = false,
        .sanitizer_name = NULL,
        .taint_origin = left.is_tainted ? left.taint_origin : right.taint_origin,
        .sanitize_point = {0},
    };
    return result;
}

TaintInfo taint_propagate_call(TaintInfo* args, size_t arg_count,
                               const SecurityContext* callee) {
    // If the callee is a sanitizer, output is clean
    if (callee && callee->is_sanitizer) {
        // Find the tainted argument to record origin
        for (size_t i = 0; i < arg_count; i++) {
            if (args[i].is_tainted) {
                Position sanitize_pos = {0}; // Caller should set this
                return taint_info_sanitized(args[i], callee->sanitizer_name,
                                            sanitize_pos);
            }
        }
        return taint_info_none();
    }

    // If callee is @trusted, output is clean regardless
    if (callee && callee->is_trusted) {
        return taint_info_none();
    }

    // Otherwise, taint propagates: if any arg is tainted, result is tainted
    uint32_t combined_sources = 0;
    Position first_origin = {0};
    bool found_taint = false;

    for (size_t i = 0; i < arg_count; i++) {
        if (args[i].is_tainted) {
            combined_sources |= args[i].sources;
            if (!found_taint) {
                first_origin = args[i].taint_origin;
                found_taint = true;
            }
        }
    }

    if (!found_taint) return taint_info_none();

    return (TaintInfo){
        .sources = combined_sources,
        .is_tainted = true,
        .is_sanitized = false,
        .sanitizer_name = NULL,
        .taint_origin = first_origin,
        .sanitize_point = {0},
    };
}

TaintInfo taint_propagate_field_access(TaintInfo base) {
    // Field access preserves taint status of the base object
    return base;
}

// =============================================================================
// Security Checking
// =============================================================================

static const char* taint_source_name(uint32_t sources) {
    if (sources & TAINT_SOURCE_USER_INPUT) return "user input";
    if (sources & TAINT_SOURCE_NETWORK) return "network";
    if (sources & TAINT_SOURCE_FILE) return "file";
    if (sources & TAINT_SOURCE_ENV) return "environment variable";
    if (sources & TAINT_SOURCE_DATABASE) return "database";
    if (sources & TAINT_SOURCE_EXTERNAL) return "external source";
    return "unknown source";
}

static const char* sink_name(SensitiveSink sink) {
    switch (sink) {
        case SINK_SQL_QUERY:     return "SQL query";
        case SINK_SHELL_COMMAND: return "shell command";
        case SINK_FILE_PATH:     return "file path";
        case SINK_NETWORK_OUTPUT:return "network output";
        case SINK_CRYPTO_KEY:    return "cryptographic key material";
        case SINK_MEMORY_ADDRESS:return "memory address";
        case SINK_FORMAT_STRING: return "format string";
        default:                 return "sensitive operation";
    }
}

void security_check_sink(SecurityAnalyzer* analyzer, TaintInfo value,
                         SensitiveSink sink, Position pos) {
    if (!analyzer || !analyzer->enable_taint_analysis) return;
    if (!value.is_tainted) return;

    char message[512];
    snprintf(message, sizeof(message),
             "Tainted data from %s flows into %s without sanitization",
             taint_source_name(value.sources), sink_name(sink));

    char suggestion[256];
    snprintf(suggestion, sizeof(suggestion),
             "Pass the value through a sanitizer function before using it in a %s",
             sink_name(sink));

    SecurityFinding finding = {
        .kind = FINDING_TAINT_FLOW,
        .severity = analyzer->strict_mode ? SEVERITY_ERROR : SEVERITY_WARNING,
        .message = strdup(message),
        .suggestion = strdup(suggestion),
        .location = pos,
        .related_location = value.taint_origin,
        .next = NULL,
    };

    security_add_finding(analyzer, finding);
    analyzer->stats.taint_flows_detected++;
}

void security_check_capability(SecurityAnalyzer* analyzer,
                               const SecurityContext* caller,
                               const SecurityContext* callee,
                               Position call_pos) {
    if (!analyzer || !analyzer->enable_capability_checking) return;
    if (!callee || callee->required_cap_count == 0) return;

    for (size_t i = 0; i < callee->required_cap_count; i++) {
        Capability required = callee->required_caps[i];
        bool satisfied = false;

        // Check if caller has the required capability
        if (caller) {
            for (size_t j = 0; j < caller->granted_cap_count; j++) {
                if (caller->granted_caps[j].kind == required.kind) {
                    // Check restriction compatibility
                    if (!required.restriction || !caller->granted_caps[j].restriction ||
                        strstr(caller->granted_caps[j].restriction,
                               required.restriction) != NULL) {
                        satisfied = true;
                        break;
                    }
                }
            }
        }

        if (!satisfied) {
            char message[512];
            snprintf(message, sizeof(message),
                     "Function requires capability '%s' which the caller does not have",
                     required.restriction ? required.restriction : "unknown");

            SecurityFinding finding = {
                .kind = FINDING_MISSING_CAPABILITY,
                .severity = SEVERITY_ERROR,
                .message = strdup(message),
                .suggestion = strdup("Add the required @capability annotation to the calling function"),
                .location = call_pos,
                .related_location = required.declared_at,
                .next = NULL,
            };

            security_add_finding(analyzer, finding);
        }

        analyzer->stats.capabilities_verified++;
    }
}

// =============================================================================
// Security Analyzer Lifecycle
// =============================================================================

SecurityAnalyzer* security_analyzer_new(void) {
    SecurityAnalyzer* analyzer = calloc(1, sizeof(SecurityAnalyzer));
    if (!analyzer) return NULL;

    analyzer->enable_taint_analysis = true;
    analyzer->enable_capability_checking = true;
    analyzer->enable_audit_reporting = true;
    analyzer->strict_mode = false;

    return analyzer;
}

void security_analyzer_free(SecurityAnalyzer* analyzer) {
    if (!analyzer) return;

    SecurityFinding* f = analyzer->findings;
    while (f) {
        SecurityFinding* next = f->next;
        free((void*)f->message);
        free((void*)f->suggestion);
        free(f);
        f = next;
    }

    free(analyzer);
}

void security_analyzer_enable_taint(SecurityAnalyzer* analyzer, bool enable) {
    if (analyzer) analyzer->enable_taint_analysis = enable;
}

void security_analyzer_enable_capabilities(SecurityAnalyzer* analyzer, bool enable) {
    if (analyzer) analyzer->enable_capability_checking = enable;
}

void security_analyzer_set_strict(SecurityAnalyzer* analyzer, bool strict) {
    if (analyzer) analyzer->strict_mode = strict;
}

// =============================================================================
// Security Context
// =============================================================================

SecurityContext* security_context_new(void) {
    return calloc(1, sizeof(SecurityContext));
}

void security_context_free(SecurityContext* ctx) {
    if (!ctx) return;
    free(ctx->required_caps);
    free(ctx->granted_caps);
    free(ctx);
}

// =============================================================================
// Annotation Parsing Helpers
// =============================================================================

bool security_parse_tainted_annotation(const char* args, TaintSource* out_source) {
    if (!out_source) return false;

    *out_source = TAINT_SOURCE_EXTERNAL; // Default

    if (!args || strlen(args) == 0) return true;

    if (strcmp(args, "user_input") == 0 || strcmp(args, "user") == 0) {
        *out_source = TAINT_SOURCE_USER_INPUT;
    } else if (strcmp(args, "network") == 0 || strcmp(args, "net") == 0) {
        *out_source = TAINT_SOURCE_NETWORK;
    } else if (strcmp(args, "file") == 0) {
        *out_source = TAINT_SOURCE_FILE;
    } else if (strcmp(args, "env") == 0 || strcmp(args, "environment") == 0) {
        *out_source = TAINT_SOURCE_ENV;
    } else if (strcmp(args, "database") == 0 || strcmp(args, "db") == 0) {
        *out_source = TAINT_SOURCE_DATABASE;
    } else if (strcmp(args, "external") == 0) {
        *out_source = TAINT_SOURCE_EXTERNAL;
    } else {
        return false; // Unknown source
    }

    return true;
}

bool security_parse_capability_annotation(const char* args, Capability* out_cap) {
    if (!args || !out_cap) return false;

    memset(out_cap, 0, sizeof(Capability));

    // Parse "kind=restriction" format, e.g. "filesystem.read=/tmp/"
    const char* dot = strchr(args, '.');
    const char* eq = strchr(args, '=');

    if (strncmp(args, "filesystem.read", 15) == 0 || strncmp(args, "file.read", 9) == 0) {
        out_cap->kind = CAP_FILE_READ;
    } else if (strncmp(args, "filesystem.write", 16) == 0 || strncmp(args, "file.write", 10) == 0) {
        out_cap->kind = CAP_FILE_WRITE;
    } else if (strncmp(args, "network.connect", 15) == 0 || strncmp(args, "net.connect", 11) == 0) {
        out_cap->kind = CAP_NETWORK_CONNECT;
    } else if (strncmp(args, "network.listen", 14) == 0 || strncmp(args, "net.listen", 10) == 0) {
        out_cap->kind = CAP_NETWORK_LISTEN;
    } else if (strncmp(args, "process.spawn", 13) == 0) {
        out_cap->kind = CAP_PROCESS_SPAWN;
    } else if (strncmp(args, "env.read", 8) == 0) {
        out_cap->kind = CAP_ENV_READ;
    } else if (strncmp(args, "env.write", 9) == 0) {
        out_cap->kind = CAP_ENV_WRITE;
    } else if (strncmp(args, "memory.unsafe", 13) == 0) {
        out_cap->kind = CAP_MEMORY_UNSAFE;
    } else if (strncmp(args, "ffi", 3) == 0) {
        out_cap->kind = CAP_FFI_CALL;
    } else {
        (void)dot;
        return false;
    }

    // Extract restriction after '='
    if (eq) {
        out_cap->restriction = eq + 1;
    }

    return true;
}

// =============================================================================
// Findings Management
// =============================================================================

void security_add_finding(SecurityAnalyzer* analyzer, SecurityFinding finding) {
    if (!analyzer) return;

    SecurityFinding* f = malloc(sizeof(SecurityFinding));
    if (!f) return;

    *f = finding;
    f->next = analyzer->findings;
    analyzer->findings = f;
    analyzer->finding_count++;
}

static const char* severity_str(int severity) {
    switch (severity) {
        case SEVERITY_ERROR:   return "ERROR";
        case SEVERITY_WARNING: return "WARNING";
        case SEVERITY_INFO:    return "INFO";
        default:               return "UNKNOWN";
    }
}

static const char* finding_kind_str(int kind) {
    switch (kind) {
        case FINDING_TAINT_FLOW:         return "taint-flow";
        case FINDING_MISSING_CAPABILITY: return "missing-capability";
        case FINDING_UNSAFE_OPERATION:   return "unsafe-operation";
        case FINDING_UNAUDITED_CRITICAL: return "unaudited-critical";
        case FINDING_WEAK_CRYPTO:        return "weak-crypto";
        case FINDING_HARDCODED_SECRET:   return "hardcoded-secret";
        default:                         return "unknown";
    }
}

void security_report_findings(SecurityAnalyzer* analyzer, FILE* output) {
    if (!analyzer || !output) return;

    if (analyzer->finding_count == 0) {
        fprintf(output, "Security analysis: no findings.\n");
        return;
    }

    fprintf(output, "\n=== Security Analysis Report ===\n");
    fprintf(output, "Findings: %zu\n\n", analyzer->finding_count);

    size_t index = 1;
    for (SecurityFinding* f = analyzer->findings; f; f = f->next, index++) {
        fprintf(output, "[%s] %s (#%zu)\n", severity_str(f->severity),
                finding_kind_str(f->kind), index);

        if (f->location.filename) {
            fprintf(output, "  --> %s:%d:%d\n",
                    f->location.filename, f->location.line, f->location.column);
        }

        if (f->message) {
            fprintf(output, "  %s\n", f->message);
        }

        if (f->related_location.filename) {
            fprintf(output, "  note: taint originated at %s:%d:%d\n",
                    f->related_location.filename,
                    f->related_location.line,
                    f->related_location.column);
        }

        if (f->suggestion) {
            fprintf(output, "  suggestion: %s\n", f->suggestion);
        }

        fprintf(output, "\n");
    }

    // Summary statistics
    fprintf(output, "--- Statistics ---\n");
    fprintf(output, "  Taint flows detected:     %zu\n", analyzer->stats.taint_flows_detected);
    fprintf(output, "  Capabilities verified:    %zu\n", analyzer->stats.capabilities_verified);
    fprintf(output, "  Sanitizations tracked:    %zu\n", analyzer->stats.sanitizations_tracked);
    fprintf(output, "  Critical functions found:  %zu\n", analyzer->stats.critical_functions_found);
    fprintf(output, "  Unsafe operations found:   %zu\n", analyzer->stats.unsafe_operations_found);
}

bool security_has_errors(SecurityAnalyzer* analyzer) {
    if (!analyzer) return false;

    for (SecurityFinding* f = analyzer->findings; f; f = f->next) {
        if (f->severity == SEVERITY_ERROR) return true;
    }

    return false;
}
