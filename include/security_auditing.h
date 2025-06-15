#ifndef GOO_SECURITY_AUDITING_H
#define GOO_SECURITY_AUDITING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include "security_framework.h"
#include "taint_analysis.h"
#include "capability_security.h"
#include "ergonomic_errors.h"
#include "ast.h"

// Forward declarations
typedef struct SecurityAuditSystem SecurityAuditSystem;
typedef struct AuditEvent AuditEvent;
typedef struct AuditLogger AuditLogger;
typedef struct AuditAnalyzer AuditAnalyzer;
typedef struct ComplianceChecker ComplianceChecker;
typedef struct ThreatDetector ThreatDetector;

// Security event types for comprehensive auditing
typedef enum {
    AUDIT_EVENT_SECURITY_VIOLATION = 1000,
    AUDIT_EVENT_CAPABILITY_GRANT,
    AUDIT_EVENT_CAPABILITY_DENY,
    AUDIT_EVENT_CAPABILITY_REVOKE,
    AUDIT_EVENT_CAPABILITY_USE,
    AUDIT_EVENT_CAPABILITY_DELEGATE,
    AUDIT_EVENT_TAINT_PROPAGATION,
    AUDIT_EVENT_TAINT_SANITIZATION,
    AUDIT_EVENT_TAINT_VIOLATION,
    AUDIT_EVENT_AUTHENTICATION_SUCCESS,
    AUDIT_EVENT_AUTHENTICATION_FAILURE,
    AUDIT_EVENT_AUTHORIZATION_SUCCESS,
    AUDIT_EVENT_AUTHORIZATION_FAILURE,
    AUDIT_EVENT_PRIVILEGE_ESCALATION,
    AUDIT_EVENT_DATA_ACCESS,
    AUDIT_EVENT_DATA_MODIFICATION,
    AUDIT_EVENT_SYSTEM_CONFIGURATION,
    AUDIT_EVENT_NETWORK_CONNECTION,
    AUDIT_EVENT_FILE_ACCESS,
    AUDIT_EVENT_PROCESS_CREATION,
    AUDIT_EVENT_CRYPTO_OPERATION,
    AUDIT_EVENT_KEY_GENERATION,
    AUDIT_EVENT_CERTIFICATE_VALIDATION,
    AUDIT_EVENT_POLICY_VIOLATION,
    AUDIT_EVENT_ANOMALY_DETECTED,
    AUDIT_EVENT_ATTACK_ATTEMPT,
    AUDIT_EVENT_COMPLIANCE_CHECK,
    AUDIT_EVENT_SECURITY_SCAN,
    AUDIT_EVENT_INCIDENT_RESPONSE,
    AUDIT_EVENT_SYSTEM_START,
    AUDIT_EVENT_SYSTEM_SHUTDOWN,
    AUDIT_EVENT_CONFIG_CHANGE,
    AUDIT_EVENT_USER_ACTION,
    AUDIT_EVENT_ADMIN_ACTION,
    AUDIT_EVENT_EMERGENCY_ACCESS,
    AUDIT_EVENT_BACKUP_OPERATION,
    AUDIT_EVENT_RESTORE_OPERATION,
    AUDIT_EVENT_AUDIT_LOG_ACCESS,
    AUDIT_EVENT_CUSTOM = 9999
} AuditEventType;

// Security severity levels
typedef enum {
    AUDIT_SEVERITY_TRACE,
    AUDIT_SEVERITY_DEBUG,
    AUDIT_SEVERITY_INFO,
    AUDIT_SEVERITY_NOTICE,
    AUDIT_SEVERITY_WARNING,
    AUDIT_SEVERITY_ERROR,
    AUDIT_SEVERITY_CRITICAL,
    AUDIT_SEVERITY_ALERT,
    AUDIT_SEVERITY_EMERGENCY
} AuditSeverity;

// Audit log destinations
typedef enum {
    AUDIT_DEST_NONE = 0,
    AUDIT_DEST_FILE = 1 << 0,
    AUDIT_DEST_SYSLOG = 1 << 1,
    AUDIT_DEST_NETWORK = 1 << 2,
    AUDIT_DEST_DATABASE = 1 << 3,
    AUDIT_DEST_MEMORY = 1 << 4,
    AUDIT_DEST_CONSOLE = 1 << 5,
    AUDIT_DEST_CUSTOM = 1 << 6
} AuditDestination;

// Audit event structure
typedef struct AuditEvent {
    uint64_t event_id;
    uint64_t timestamp_ns;
    AuditEventType event_type;
    AuditSeverity severity;
    
    // Event source information
    char source_module[128];
    char source_function[128];
    char source_file[256];
    uint32_t source_line;
    
    // Security context
    char user_id[64];
    char session_id[64];
    char process_id[32];
    char thread_id[32];
    
    // Event details
    char event_title[128];
    char event_description[512];
    char event_category[64];
    
    // Security-specific data
    SecurityCapability involved_capabilities;
    TaintLevel involved_taint_level;
    char security_policy[64];
    char compliance_framework[64];
    
    // Risk assessment
    enum {
        RISK_NONE,
        RISK_LOW,
        RISK_MEDIUM,
        RISK_HIGH,
        RISK_CRITICAL
    } risk_level;
    
    double risk_score;          // 0.0 to 10.0
    double confidence;          // 0.0 to 1.0
    
    // Attack vectors and indicators
    char attack_vector[128];
    char attack_indicators[256];
    char mitigation_actions[256];
    
    // Structured data
    char* key_value_pairs;      // JSON-formatted additional data
    size_t data_size;
    
    // Correlation data
    uint64_t correlation_id;    // Links related events
    uint64_t parent_event_id;   // Parent event for event chains
    uint64_t* related_events;   // Array of related event IDs
    size_t related_count;
    
    // Compliance and forensics
    char compliance_status[32]; // COMPLIANT, NON_COMPLIANT, UNKNOWN
    bool requires_investigation;
    bool is_security_incident;
    bool has_been_reviewed;
    char reviewer[64];
    uint64_t review_time;
    
    // Performance data
    uint64_t processing_time_ns;
    uint64_t detection_latency_ns;
    
    struct AuditEvent* next;
} AuditEvent;

// Real-time threat detection patterns
typedef struct ThreatPattern {
    char pattern_name[128];
    char description[256];
    
    // Pattern matching criteria
    AuditEventType* event_types;
    size_t event_type_count;
    
    // Time-based criteria
    uint64_t time_window_ms;
    uint32_t min_occurrences;
    uint32_t max_occurrences;
    
    // Severity thresholds
    AuditSeverity min_severity;
    double min_risk_score;
    
    // Pattern detection logic
    bool (*pattern_matcher)(const AuditEvent* events, size_t count, void* context);
    void* matcher_context;
    
    // Response actions
    enum {
        RESPONSE_LOG_ONLY,
        RESPONSE_ALERT,
        RESPONSE_BLOCK,
        RESPONSE_QUARANTINE,
        RESPONSE_ESCALATE,
        RESPONSE_CUSTOM
    } response_action;
    
    void (*response_handler)(const struct ThreatPattern* pattern, const AuditEvent* trigger_event, void* context);
    void* response_context;
    
    // Pattern metadata
    bool is_enabled;
    double false_positive_rate;
    uint64_t last_triggered;
    uint64_t trigger_count;
    
    struct ThreatPattern* next;
} ThreatPattern;

// Security compliance framework
typedef struct ComplianceRule {
    char rule_id[64];
    char rule_name[128];
    char framework[64];         // SOC2, ISO27001, GDPR, etc.
    char description[512];
    
    // Rule criteria
    AuditEventType required_event_type;
    AuditSeverity min_severity;
    uint64_t retention_period_days;
    
    // Validation logic
    bool (*compliance_checker)(const AuditEvent* event, void* context);
    void* checker_context;
    
    // Remediation
    char remediation_steps[512];
    bool auto_remediation;
    void (*remediation_handler)(const struct ComplianceRule* rule, const AuditEvent* event, void* context);
    
    bool is_mandatory;
    bool is_enabled;
    
    struct ComplianceRule* next;
} ComplianceRule;

// Audit analyzer for pattern detection and analytics
typedef struct AuditAnalyzer {
    // Threat detection patterns
    ThreatPattern* threat_patterns;
    size_t pattern_count;
    
    // Statistical analysis
    struct {
        uint64_t total_events;
        uint64_t events_by_type[40];    // Indexed by AuditEventType
        uint64_t events_by_severity[9]; // Indexed by AuditSeverity
        
        // Time-based statistics
        uint64_t events_last_hour;
        uint64_t events_last_day;
        uint64_t events_last_week;
        
        // Security metrics
        uint64_t security_violations;
        uint64_t policy_violations;
        uint64_t compliance_violations;
        uint64_t potential_attacks;
        uint64_t blocked_actions;
        
        // Performance metrics
        uint64_t avg_processing_time_ns;
        uint64_t max_processing_time_ns;
        double throughput_events_per_second;
    } statistics;
    
    // Anomaly detection
    struct {
        bool enable_anomaly_detection;
        double baseline_event_rate;
        double anomaly_threshold;      // Standard deviations from baseline
        uint64_t learning_period_ms;
        uint64_t detection_window_ms;
        
        // Machine learning model (simplified)
        bool model_trained;
        double feature_weights[32];
        size_t feature_count;
    } anomaly_detector;
    
    // Event correlation
    struct {
        bool enable_correlation;
        uint64_t correlation_window_ms;
        uint32_t max_correlation_depth;
        
        // Correlation rules
        struct {
            char rule_name[64];
            AuditEventType* event_sequence;
            size_t sequence_length;
            uint64_t max_time_span_ms;
            bool (*correlation_logic)(const AuditEvent* events, size_t count);
        } *correlation_rules;
        size_t correlation_rule_count;
    } correlator;
    
    pthread_mutex_t analyzer_mutex;
} AuditAnalyzer;

// Multi-destination audit logger
typedef struct AuditLogger {
    // Destination configuration
    uint32_t enabled_destinations;
    
    // File logging
    struct {
        char log_file_path[512];
        char backup_dir[512];
        FILE* log_file;
        uint64_t max_file_size;
        uint32_t max_backup_files;
        bool rotate_on_size;
        bool rotate_daily;
        bool compress_backups;
    } file_config;
    
    // Network logging
    struct {
        char server_host[256];
        uint16_t server_port;
        char protocol[16];          // TCP, UDP, TLS
        int socket_fd;
        bool use_encryption;
        char encryption_cert[512];
    } network_config;
    
    // Database logging
    struct {
        char connection_string[512];
        char table_name[128];
        void* db_connection;
        bool batch_inserts;
        uint32_t batch_size;
        uint32_t batch_timeout_ms;
    } database_config;
    
    // Memory buffer (for high-performance scenarios)
    struct {
        AuditEvent** event_buffer;
        size_t buffer_size;
        size_t buffer_capacity;
        size_t buffer_head;
        size_t buffer_tail;
        bool buffer_full;
        pthread_mutex_t buffer_mutex;
    } memory_config;
    
    // Logging policies
    struct {
        bool async_logging;
        bool buffer_events;
        uint32_t flush_interval_ms;
        AuditSeverity min_log_level;
        
        // Filtering
        AuditEventType* filtered_types;
        size_t filtered_type_count;
        bool use_sampling;
        double sampling_rate;
        
        // Privacy and redaction
        bool redact_sensitive_data;
        char* sensitive_field_patterns;
        size_t pattern_count;
    } policy;
    
    // Performance tracking
    struct {
        uint64_t events_logged;
        uint64_t events_dropped;
        uint64_t log_errors;
        uint64_t avg_log_time_ns;
        uint64_t total_log_time_ns;
    } performance;
    
    pthread_mutex_t logger_mutex;
    pthread_t logging_thread;
    bool is_active;
} AuditLogger;

// Compliance checker for regulatory frameworks
typedef struct ComplianceChecker {
    // Supported frameworks
    ComplianceRule* rules;
    size_t rule_count;
    
    // Framework configurations
    struct {
        char framework_name[64];
        bool is_enabled;
        AuditSeverity required_log_level;
        uint64_t retention_period_days;
        bool require_encryption;
        bool require_digital_signatures;
        bool require_immutable_logs;
    } *frameworks;
    size_t framework_count;
    
    // Compliance monitoring
    struct {
        uint64_t total_checks;
        uint64_t compliance_passes;
        uint64_t compliance_failures;
        uint64_t policy_violations;
        
        // Current compliance status
        double overall_compliance_score;    // 0.0 to 100.0
        char last_assessment_time[32];
        bool is_compliant;
        
        // Non-compliance tracking
        struct {
            char violation_type[128];
            char description[256];
            uint64_t occurrence_time;
            bool is_resolved;
        } *violations;
        size_t violation_count;
    } status;
    
    pthread_mutex_t compliance_mutex;
} ComplianceChecker;

// Threat detector for real-time security monitoring
typedef struct ThreatDetector {
    AuditAnalyzer* analyzer;
    
    // Detection engine
    struct {
        bool enable_real_time_detection;
        bool enable_behavioral_analysis;
        bool enable_signature_detection;
        bool enable_heuristic_detection;
        
        // Detection thresholds
        double threat_threshold;
        double anomaly_threshold;
        uint32_t correlation_depth;
        uint64_t analysis_window_ms;
    } config;
    
    // Active threats
    struct {
        uint64_t threat_id;
        char threat_type[128];
        char description[256];
        double severity_score;
        uint64_t first_detected;
        uint64_t last_activity;
        bool is_active;
        
        // Attack characteristics
        char attack_vector[128];
        char target_assets[256];
        char indicators[512];
        
        // Response status
        bool response_triggered;
        char response_actions[256];
        bool is_contained;
    } *active_threats;
    size_t active_threat_count;
    size_t threat_capacity;
    
    // Machine learning model for behavioral analysis
    struct {
        bool model_enabled;
        bool model_trained;
        char model_file[512];
        
        // Feature extraction
        double* feature_vector;
        size_t feature_count;
        
        // Model parameters
        double learning_rate;
        double regularization;
        uint32_t training_iterations;
    } ml_model;
    
    // Incident response integration
    struct {
        bool auto_response_enabled;
        char response_webhook_url[512];
        char escalation_email[256];
        
        void (*incident_handler)(const char* threat_type, double severity, void* context);
        void* handler_context;
    } incident_response;
    
    pthread_mutex_t detector_mutex;
} ThreatDetector;

// Main security audit system
typedef struct SecurityAuditSystem {
    SecurityContext* security_context;
    
    // Core components
    AuditLogger* logger;
    AuditAnalyzer* analyzer;
    ComplianceChecker* compliance_checker;
    ThreatDetector* threat_detector;
    
    // Event processing
    struct {
        AuditEvent** event_queue;
        size_t queue_size;
        size_t queue_capacity;
        size_t queue_head;
        size_t queue_tail;
        
        // Processing threads
        pthread_t* processing_threads;
        size_t thread_count;
        bool processing_active;
        
        // Flow control
        uint32_t max_events_per_second;
        uint32_t current_event_rate;
        bool rate_limiting_enabled;
        
        pthread_mutex_t queue_mutex;
        pthread_cond_t queue_not_empty;
        pthread_cond_t queue_not_full;
    } event_processing;
    
    // Integration with other security systems
    TaintAnalyzer* taint_analyzer;
    CapabilitySystem* capability_system;
    
    // System configuration
    struct {
        bool audit_enabled;
        bool real_time_processing;
        bool store_events_in_memory;
        bool enable_compression;
        bool enable_encryption;
        
        // Performance tuning
        uint32_t event_buffer_size;
        uint32_t processing_threads;
        uint32_t analysis_interval_ms;
        
        // Security settings
        bool tamper_protection;
        bool integrity_checking;
        char audit_signing_key[512];
    } config;
    
    // System statistics
    struct {
        uint64_t total_events_processed;
        uint64_t events_per_second;
        uint64_t processing_errors;
        uint64_t storage_errors;
        
        // Security statistics
        uint64_t threats_detected;
        uint64_t incidents_triggered;
        uint64_t compliance_violations;
        uint64_t policy_violations;
        
        // Performance statistics
        uint64_t avg_processing_time_ns;
        uint64_t max_queue_depth;
        double cpu_usage_percent;
        uint64_t memory_usage_bytes;
    } statistics;
    
    // Lifecycle management
    bool is_initialized;
    bool is_running;
    atomic_bool shutdown_requested;
    
    pthread_mutex_t system_mutex;
} SecurityAuditSystem;

// Core audit system operations
SecurityAuditSystem* security_audit_system_create(SecurityContext* security_context);
void security_audit_system_destroy(SecurityAuditSystem* system);
Result_void_ptr security_audit_system_initialize(SecurityAuditSystem* system);
Result_void_ptr security_audit_system_start(SecurityAuditSystem* system);
Result_void_ptr security_audit_system_stop(SecurityAuditSystem* system);

// Component creation and destruction
AuditLogger* audit_logger_create(void);
void audit_logger_destroy(AuditLogger* logger);
AuditAnalyzer* audit_analyzer_create(void);
void audit_analyzer_destroy(AuditAnalyzer* analyzer);
ComplianceChecker* compliance_checker_create(void);
void compliance_checker_destroy(ComplianceChecker* checker);
ThreatDetector* threat_detector_create(void);
void threat_detector_destroy(ThreatDetector* detector);

// Event creation and logging
AuditEvent* audit_event_create(AuditEventType event_type, AuditSeverity severity, const char* title);
void audit_event_destroy(AuditEvent* event);
Result_void_ptr audit_event_set_details(AuditEvent* event, const char* description, const char* category);
Result_void_ptr audit_event_set_security_context(AuditEvent* event, SecurityCapability capabilities, TaintLevel taint_level);
Result_void_ptr audit_event_set_source_info(AuditEvent* event, const char* module, const char* function, const char* file, uint32_t line);
Result_void_ptr audit_event_add_correlation(AuditEvent* event, uint64_t correlation_id, uint64_t parent_id);

// Main auditing functions
Result_void_ptr security_audit_log_event(SecurityAuditSystem* system, AuditEvent* event);
Result_void_ptr security_audit_log_simple(SecurityAuditSystem* system, AuditEventType type, AuditSeverity severity, const char* message);
Result_void_ptr security_audit_log_with_context(SecurityAuditSystem* system, AuditEventType type, const char* user, const char* action, const char* resource);

// Integration functions
Result_void_ptr security_audit_integrate_taint_analysis(SecurityAuditSystem* system, TaintAnalyzer* taint_analyzer);
Result_void_ptr security_audit_integrate_capability_system(SecurityAuditSystem* system, CapabilitySystem* capability_system);
Result_void_ptr security_audit_log_taint_violation(SecurityAuditSystem* system, const char* location, TaintLevel taint_level, const char* operation);
Result_void_ptr security_audit_log_capability_event(SecurityAuditSystem* system, SecurityCapability capability, const char* entity, bool granted);

// Threat detection and analysis
Result_void_ptr security_audit_add_threat_pattern(SecurityAuditSystem* system, ThreatPattern* pattern);
Result_void_ptr security_audit_analyze_events(SecurityAuditSystem* system, uint64_t time_window_ms);
Result_void_ptr security_audit_detect_anomalies(SecurityAuditSystem* system);
bool security_audit_check_threat_patterns(SecurityAuditSystem* system, const AuditEvent* event);

// Compliance checking
Result_void_ptr security_audit_add_compliance_rule(SecurityAuditSystem* system, ComplianceRule* rule);
Result_void_ptr security_audit_check_compliance(SecurityAuditSystem* system, const char* framework);
double security_audit_get_compliance_score(SecurityAuditSystem* system, const char* framework);

// Reporting and analytics
typedef struct SecurityAuditReport {
    uint64_t report_generation_time;
    uint64_t report_period_start;
    uint64_t report_period_end;
    
    // Event statistics
    uint64_t total_events;
    uint64_t events_by_severity[9];
    uint64_t events_by_type[40];
    
    // Security metrics
    uint64_t security_violations;
    uint64_t policy_violations;
    uint64_t threats_detected;
    uint64_t incidents_responded;
    
    // Compliance status
    double overall_compliance_score;
    struct {
        char framework_name[64];
        double compliance_score;
        bool is_compliant;
        uint64_t violations;
    } *framework_status;
    size_t framework_count;
    
    // Top security events
    struct {
        AuditEventType event_type;
        uint64_t count;
        double risk_score;
        char description[256];
    } top_events[10];
    
    // Recommendations
    char* recommendations;
    size_t recommendation_count;
} SecurityAuditReport;

SecurityAuditReport* security_audit_generate_report(SecurityAuditSystem* system, uint64_t start_time, uint64_t end_time);
void security_audit_report_destroy(SecurityAuditReport* report);
Result_void_ptr security_audit_export_report(SecurityAuditReport* report, const char* format, const char* output_file);

// Built-in threat patterns
ThreatPattern* create_brute_force_pattern(void);
ThreatPattern* create_privilege_escalation_pattern(void);
ThreatPattern* create_data_exfiltration_pattern(void);
ThreatPattern* create_anomalous_access_pattern(void);
ThreatPattern* create_injection_attack_pattern(void);

// Built-in compliance rules
ComplianceRule* create_soc2_logging_rule(void);
ComplianceRule* create_gdpr_data_access_rule(void);
ComplianceRule* create_iso27001_access_control_rule(void);
ComplianceRule* create_hipaa_audit_rule(void);

// Configuration helpers
typedef struct SecurityAuditConfig {
    bool enable_real_time_processing;
    bool enable_threat_detection;
    bool enable_compliance_checking;
    bool enable_anomaly_detection;
    
    uint32_t max_events_per_second;
    uint32_t processing_threads;
    uint64_t event_retention_days;
    
    AuditDestination log_destinations;
    AuditSeverity min_log_level;
    
    bool enable_encryption;
    bool enable_compression;
    bool enable_integrity_checking;
} SecurityAuditConfig;

SecurityAuditConfig security_audit_config_default(void);
SecurityAuditConfig security_audit_config_high_security(void);
SecurityAuditConfig security_audit_config_high_performance(void);
Result_void_ptr security_audit_system_configure(SecurityAuditSystem* system, SecurityAuditConfig config);

// Utility macros for easy auditing
#define AUDIT_SECURITY_VIOLATION(system, msg) \
    security_audit_log_simple((system), AUDIT_EVENT_SECURITY_VIOLATION, AUDIT_SEVERITY_CRITICAL, (msg))

#define AUDIT_CAPABILITY_USE(system, cap, entity) \
    security_audit_log_capability_event((system), (cap), (entity), true)

#define AUDIT_TAINT_VIOLATION(system, location, level, op) \
    security_audit_log_taint_violation((system), (location), (level), (op))

#define AUDIT_USER_ACTION(system, user, action, resource) \
    security_audit_log_with_context((system), AUDIT_EVENT_USER_ACTION, (user), (action), (resource))

// Error codes specific to security auditing
#define ERROR_AUDIT_SYSTEM_FAILED       0x8001
#define ERROR_AUDIT_LOG_FULL            0x8002
#define ERROR_AUDIT_DESTINATION_FAILED  0x8003
#define ERROR_THREAT_DETECTION_FAILED   0x8004
#define ERROR_COMPLIANCE_CHECK_FAILED   0x8005
#define ERROR_AUDIT_CORRELATION_FAILED  0x8006
#define ERROR_AUDIT_ENCRYPTION_FAILED   0x8007

#endif // GOO_SECURITY_AUDITING_H