#ifndef GOO_SECURITY_H
#define GOO_SECURITY_H

#include "ast.h"
#include "types.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// =============================================================================
// Taint Analysis System
// =============================================================================

// Taint sources — where untrusted data originates
typedef enum {
    TAINT_SOURCE_NONE       = 0,
    TAINT_SOURCE_USER_INPUT = 1 << 0,
    TAINT_SOURCE_NETWORK    = 1 << 1,
    TAINT_SOURCE_FILE       = 1 << 2,
    TAINT_SOURCE_ENV        = 1 << 3,
    TAINT_SOURCE_DATABASE   = 1 << 4,
    TAINT_SOURCE_EXTERNAL   = 1 << 5,
} TaintSource;

// Taint state for a value
typedef struct TaintInfo {
    uint32_t sources;              // Bitmask of TaintSource
    bool is_tainted;               // Quick check
    bool is_sanitized;             // Has been through a sanitizer
    const char* sanitizer_name;    // Which sanitizer was applied
    Position taint_origin;         // Where the taint was introduced
    Position sanitize_point;       // Where it was sanitized
} TaintInfo;

// Sensitive sink categories — operations that require clean data
typedef enum {
    SINK_NONE            = 0,
    SINK_SQL_QUERY       = 1 << 0,
    SINK_SHELL_COMMAND   = 1 << 1,
    SINK_FILE_PATH       = 1 << 2,
    SINK_NETWORK_OUTPUT  = 1 << 3,
    SINK_CRYPTO_KEY      = 1 << 4,
    SINK_MEMORY_ADDRESS  = 1 << 5,
    SINK_FORMAT_STRING   = 1 << 6,
} SensitiveSink;

// =============================================================================
// Capability-Based Security
// =============================================================================

typedef enum {
    CAP_NONE             = 0,
    CAP_FILE_READ        = 1 << 0,
    CAP_FILE_WRITE       = 1 << 1,
    CAP_NETWORK_CONNECT  = 1 << 2,
    CAP_NETWORK_LISTEN   = 1 << 3,
    CAP_PROCESS_SPAWN    = 1 << 4,
    CAP_ENV_READ         = 1 << 5,
    CAP_ENV_WRITE        = 1 << 6,
    CAP_MEMORY_UNSAFE    = 1 << 7,
    CAP_FFI_CALL         = 1 << 8,
} CapabilityKind;

typedef struct Capability {
    CapabilityKind kind;
    const char* restriction;       // Path/host restriction (e.g., "/tmp/")
    Position declared_at;
} Capability;

// =============================================================================
// Security Context — per-function security metadata
// =============================================================================

typedef struct SecurityContext {
    // Capabilities this function requires
    Capability* required_caps;
    size_t required_cap_count;

    // Capabilities this function grants to callees
    Capability* granted_caps;
    size_t granted_cap_count;

    // Whether this function is a sanitizer
    bool is_sanitizer;
    const char* sanitizer_name;
    uint32_t sanitizes_sources;    // Which taint sources it cleans

    // Security audit level
    bool is_security_critical;
    bool requires_audit;

    // Trust level
    bool is_trusted;               // @trusted — bypasses taint checks
} SecurityContext;

// =============================================================================
// Security Analyzer — the main analysis engine
// =============================================================================

typedef struct SecurityFinding {
    enum {
        FINDING_TAINT_FLOW,        // Tainted data reaches sensitive sink
        FINDING_MISSING_CAPABILITY,// Calling function without required capability
        FINDING_UNSAFE_OPERATION,  // Unsafe operation outside unsafe block
        FINDING_UNAUDITED_CRITICAL,// Security-critical function not audited
        FINDING_WEAK_CRYPTO,       // Weak cryptographic operation detected
        FINDING_HARDCODED_SECRET,  // Hardcoded secret/credential
    } kind;

    enum {
        SEVERITY_ERROR,            // Must fix — compilation fails
        SEVERITY_WARNING,          // Should fix — compilation succeeds
        SEVERITY_INFO,             // Informational
    } severity;

    const char* message;
    const char* suggestion;
    Position location;
    Position related_location;     // e.g., where taint was introduced

    struct SecurityFinding* next;
} SecurityFinding;

typedef struct SecurityAnalyzer {
    // Configuration
    bool enable_taint_analysis;
    bool enable_capability_checking;
    bool enable_audit_reporting;
    bool strict_mode;              // Treat warnings as errors

    // State
    SecurityFinding* findings;
    size_t finding_count;

    // Statistics
    struct {
        size_t taint_flows_detected;
        size_t capabilities_verified;
        size_t sanitizations_tracked;
        size_t critical_functions_found;
        size_t unsafe_operations_found;
    } stats;
} SecurityAnalyzer;

// =============================================================================
// API
// =============================================================================

// Analyzer lifecycle
SecurityAnalyzer* security_analyzer_new(void);
void security_analyzer_free(SecurityAnalyzer* analyzer);

// Configuration
void security_analyzer_enable_taint(SecurityAnalyzer* analyzer, bool enable);
void security_analyzer_enable_capabilities(SecurityAnalyzer* analyzer, bool enable);
void security_analyzer_set_strict(SecurityAnalyzer* analyzer, bool strict);

// Taint analysis
TaintInfo taint_info_none(void);
TaintInfo taint_info_tainted(TaintSource source, Position origin);
TaintInfo taint_info_sanitized(TaintInfo original, const char* sanitizer, Position point);
bool taint_info_is_clean(const TaintInfo* info);

// Taint propagation rules
TaintInfo taint_propagate_binary(TaintInfo left, TaintInfo right);
TaintInfo taint_propagate_call(TaintInfo* args, size_t arg_count, const SecurityContext* callee);
TaintInfo taint_propagate_field_access(TaintInfo base);

// Security checking
void security_check_sink(SecurityAnalyzer* analyzer, TaintInfo value,
                         SensitiveSink sink, Position pos);
void security_check_capability(SecurityAnalyzer* analyzer,
                               const SecurityContext* caller,
                               const SecurityContext* callee,
                               Position call_pos);

// Annotation parsing helpers
SecurityContext* security_context_new(void);
void security_context_free(SecurityContext* ctx);
bool security_parse_tainted_annotation(const char* args, TaintSource* out_source);
bool security_parse_capability_annotation(const char* args, Capability* out_cap);

// Findings
void security_add_finding(SecurityAnalyzer* analyzer, SecurityFinding finding);
void security_report_findings(SecurityAnalyzer* analyzer, FILE* output);
bool security_has_errors(SecurityAnalyzer* analyzer);

#endif // GOO_SECURITY_H
