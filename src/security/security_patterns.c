#include "../../include/security_auditing.h"
#include <stdlib.h>
#include <string.h>

// Built-in threat pattern implementations

// Brute force attack detection
static bool brute_force_matcher(const AuditEvent* events, size_t count, void* context) {
    (void)context; // Suppress unused parameter warning
    
    if (count < 5) return false;
    
    // Look for repeated authentication failures
    uint32_t failure_count = 0;
    uint64_t time_window = 5 * 60 * 1000 * 1000000ULL; // 5 minutes in nanoseconds
    uint64_t latest_time = events[count - 1].timestamp_ns;
    
    for (size_t i = 0; i < count; i++) {
        if (events[i].event_type == AUDIT_EVENT_AUTHENTICATION_FAILURE &&
            (latest_time - events[i].timestamp_ns) <= time_window) {
            failure_count++;
        }
    }
    
    return failure_count >= 5; // 5 failures in 5 minutes
}

static void brute_force_response(const ThreatPattern* pattern, const AuditEvent* trigger_event, void* context) {
    (void)pattern;
    (void)context;
    
    // Log the threat detection
    printf("THREAT DETECTED: Brute force attack from user %s at %lu\n", 
           trigger_event->user_id, trigger_event->timestamp_ns);
    
    // In a real implementation, this would:
    // - Block the IP address
    // - Send alerts to security team
    // - Log to SIEM system
}

ThreatPattern* create_brute_force_pattern(void) {
    ThreatPattern* pattern = xcalloc(1, sizeof(ThreatPattern));
    if (!pattern) return NULL;
    
    strncpy(pattern->pattern_name, "brute_force_attack", sizeof(pattern->pattern_name) - 1);
    strncpy(pattern->description, "Detects repeated authentication failures indicating brute force attack", 
            sizeof(pattern->description) - 1);
    
    // Configure pattern
    pattern->event_type_count = 1;
    pattern->event_types = xmalloc(sizeof(AuditEventType));
    if (pattern->event_types) {
        pattern->event_types[0] = AUDIT_EVENT_AUTHENTICATION_FAILURE;
    }
    
    pattern->time_window_ms = 5 * 60 * 1000; // 5 minutes
    pattern->min_occurrences = 5;
    pattern->max_occurrences = 0; // No maximum
    pattern->min_severity = AUDIT_SEVERITY_WARNING;
    pattern->min_risk_score = 5.0;
    
    pattern->pattern_matcher = brute_force_matcher;
    pattern->response_action = RESPONSE_ALERT;
    pattern->response_handler = brute_force_response;
    
    pattern->is_enabled = true;
    pattern->false_positive_rate = 0.05; // 5%
    
    return pattern;
}

// Privilege escalation detection
static bool privilege_escalation_matcher(const AuditEvent* events, size_t count, void* context) {
    (void)context;
    
    if (count < 2) return false;
    
    // Look for capability grants followed by privilege escalation
    bool found_escalation = false;
    uint64_t time_window = 10 * 60 * 1000 * 1000000ULL; // 10 minutes in nanoseconds
    
    for (size_t i = 0; i < count - 1; i++) {
        if (events[i].event_type == AUDIT_EVENT_CAPABILITY_GRANT) {
            for (size_t j = i + 1; j < count; j++) {
                if (events[j].event_type == AUDIT_EVENT_PRIVILEGE_ESCALATION &&
                    (events[j].timestamp_ns - events[i].timestamp_ns) <= time_window &&
                    strcmp(events[i].user_id, events[j].user_id) == 0) {
                    found_escalation = true;
                    break;
                }
            }
        }
    }
    
    return found_escalation;
}

ThreatPattern* create_privilege_escalation_pattern(void) {
    ThreatPattern* pattern = xcalloc(1, sizeof(ThreatPattern));
    if (!pattern) return NULL;
    
    strncpy(pattern->pattern_name, "privilege_escalation", sizeof(pattern->pattern_name) - 1);
    strncpy(pattern->description, "Detects potential privilege escalation attacks", 
            sizeof(pattern->description) - 1);
    
    pattern->event_type_count = 2;
    pattern->event_types = malloc(2 * sizeof(AuditEventType));
    if (pattern->event_types) {
        pattern->event_types[0] = AUDIT_EVENT_CAPABILITY_GRANT;
        pattern->event_types[1] = AUDIT_EVENT_PRIVILEGE_ESCALATION;
    }
    
    pattern->time_window_ms = 10 * 60 * 1000; // 10 minutes
    pattern->min_occurrences = 1;
    pattern->min_severity = AUDIT_SEVERITY_CRITICAL;
    pattern->min_risk_score = 8.0;
    
    pattern->pattern_matcher = privilege_escalation_matcher;
    pattern->response_action = RESPONSE_BLOCK;
    
    pattern->is_enabled = true;
    pattern->false_positive_rate = 0.02; // 2%
    
    return pattern;
}

// Data exfiltration detection
static bool data_exfiltration_matcher(const AuditEvent* events, size_t count, void* context) {
    (void)context;
    
    if (count < 3) return false;
    
    // Look for pattern: data access -> large data transfer -> external network connection
    uint64_t time_window = 30 * 60 * 1000 * 1000000ULL; // 30 minutes
    uint32_t access_count = 0;
    uint32_t transfer_count = 0;
    uint32_t network_count = 0;
    
    uint64_t latest_time = events[count - 1].timestamp_ns;
    
    for (size_t i = 0; i < count; i++) {
        if ((latest_time - events[i].timestamp_ns) <= time_window) {
            switch (events[i].event_type) {
                case AUDIT_EVENT_DATA_ACCESS:
                    access_count++;
                    break;
                case AUDIT_EVENT_DATA_MODIFICATION:
                    transfer_count++;
                    break;
                case AUDIT_EVENT_NETWORK_CONNECTION:
                    network_count++;
                    break;
                default:
                    break;
            }
        }
    }
    
    return (access_count >= 5 && transfer_count >= 3 && network_count >= 2);
}

ThreatPattern* create_data_exfiltration_pattern(void) {
    ThreatPattern* pattern = xcalloc(1, sizeof(ThreatPattern));
    if (!pattern) return NULL;
    
    strncpy(pattern->pattern_name, "data_exfiltration", sizeof(pattern->pattern_name) - 1);
    strncpy(pattern->description, "Detects potential data exfiltration attempts", 
            sizeof(pattern->description) - 1);
    
    pattern->event_type_count = 3;
    pattern->event_types = malloc(3 * sizeof(AuditEventType));
    if (pattern->event_types) {
        pattern->event_types[0] = AUDIT_EVENT_DATA_ACCESS;
        pattern->event_types[1] = AUDIT_EVENT_DATA_MODIFICATION;
        pattern->event_types[2] = AUDIT_EVENT_NETWORK_CONNECTION;
    }
    
    pattern->time_window_ms = 30 * 60 * 1000; // 30 minutes
    pattern->min_occurrences = 3;
    pattern->min_severity = AUDIT_SEVERITY_ERROR;
    pattern->min_risk_score = 7.5;
    
    pattern->pattern_matcher = data_exfiltration_matcher;
    pattern->response_action = RESPONSE_ESCALATE;
    
    pattern->is_enabled = true;
    pattern->false_positive_rate = 0.10; // 10%
    
    return pattern;
}

// Anomalous access pattern detection
static bool anomalous_access_matcher(const AuditEvent* events, size_t count, void* context) {
    (void)context;
    
    if (count < 10) return false;
    
    // Look for unusual access patterns (access outside normal hours, unusual resources)
    uint32_t off_hours_access = 0;
    uint32_t high_risk_access = 0;
    uint64_t time_window = 60 * 60 * 1000 * 1000000ULL; // 1 hour
    uint64_t latest_time = events[count - 1].timestamp_ns;
    
    for (size_t i = 0; i < count; i++) {
        if ((latest_time - events[i].timestamp_ns) <= time_window) {
            // Convert timestamp to hours (simplified)
            uint64_t hour = (events[i].timestamp_ns / 1000000000ULL / 3600) % 24;
            
            if (hour < 6 || hour > 22) { // Outside normal business hours
                off_hours_access++;
            }
            
            if (events[i].risk_level >= RISK_HIGH) {
                high_risk_access++;
            }
        }
    }
    
    return (off_hours_access >= 5 || high_risk_access >= 3);
}

ThreatPattern* create_anomalous_access_pattern(void) {
    ThreatPattern* pattern = xcalloc(1, sizeof(ThreatPattern));
    if (!pattern) return NULL;
    
    strncpy(pattern->pattern_name, "anomalous_access", sizeof(pattern->pattern_name) - 1);
    strncpy(pattern->description, "Detects unusual access patterns that may indicate insider threats", 
            sizeof(pattern->description) - 1);
    
    pattern->event_type_count = 2;
    pattern->event_types = malloc(2 * sizeof(AuditEventType));
    if (pattern->event_types) {
        pattern->event_types[0] = AUDIT_EVENT_DATA_ACCESS;
        pattern->event_types[1] = AUDIT_EVENT_FILE_ACCESS;
    }
    
    pattern->time_window_ms = 60 * 60 * 1000; // 1 hour
    pattern->min_occurrences = 5;
    pattern->min_severity = AUDIT_SEVERITY_WARNING;
    pattern->min_risk_score = 6.0;
    
    pattern->pattern_matcher = anomalous_access_matcher;
    pattern->response_action = RESPONSE_ALERT;
    
    pattern->is_enabled = true;
    pattern->false_positive_rate = 0.15; // 15%
    
    return pattern;
}

// Injection attack detection
static bool injection_attack_matcher(const AuditEvent* events, size_t count, void* context) {
    (void)context;
    
    if (count < 1) return false;
    
    // Look for taint violations that could indicate injection attacks
    for (size_t i = 0; i < count; i++) {
        if (events[i].event_type == AUDIT_EVENT_TAINT_VIOLATION &&
            events[i].involved_taint_level >= TAINT_UNVALIDATED &&
            events[i].risk_level >= RISK_HIGH) {
            return true;
        }
    }
    
    return false;
}

ThreatPattern* create_injection_attack_pattern(void) {
    ThreatPattern* pattern = xcalloc(1, sizeof(ThreatPattern));
    if (!pattern) return NULL;
    
    strncpy(pattern->pattern_name, "injection_attack", sizeof(pattern->pattern_name) - 1);
    strncpy(pattern->description, "Detects potential injection attacks (SQL, XSS, command injection)", 
            sizeof(pattern->description) - 1);
    
    pattern->event_type_count = 1;
    pattern->event_types = xmalloc(sizeof(AuditEventType));
    if (pattern->event_types) {
        pattern->event_types[0] = AUDIT_EVENT_TAINT_VIOLATION;
    }
    
    pattern->time_window_ms = 1000; // 1 second (immediate detection)
    pattern->min_occurrences = 1;
    pattern->min_severity = AUDIT_SEVERITY_CRITICAL;
    pattern->min_risk_score = 8.5;
    
    pattern->pattern_matcher = injection_attack_matcher;
    pattern->response_action = RESPONSE_BLOCK;
    
    pattern->is_enabled = true;
    pattern->false_positive_rate = 0.01; // 1%
    
    return pattern;
}

// Built-in compliance rule implementations

// SOC 2 logging compliance checker
static bool soc2_logging_checker(const AuditEvent* event, void* context) {
    (void)context;
    
    // SOC 2 requires comprehensive logging of all access and changes
    return (event->event_type == AUDIT_EVENT_DATA_ACCESS ||
            event->event_type == AUDIT_EVENT_DATA_MODIFICATION ||
            event->event_type == AUDIT_EVENT_SYSTEM_CONFIGURATION ||
            event->event_type == AUDIT_EVENT_USER_ACTION ||
            event->event_type == AUDIT_EVENT_ADMIN_ACTION) &&
           event->severity >= AUDIT_SEVERITY_INFO;
}

ComplianceRule* create_soc2_logging_rule(void) {
    ComplianceRule* rule = xcalloc(1, sizeof(ComplianceRule));
    if (!rule) return NULL;
    
    strncpy(rule->rule_id, "SOC2-LOG-001", sizeof(rule->rule_id) - 1);
    strncpy(rule->rule_name, "SOC 2 Comprehensive Logging", sizeof(rule->rule_name) - 1);
    strncpy(rule->framework, "SOC2", sizeof(rule->framework) - 1);
    strncpy(rule->description, "All system access and changes must be logged with sufficient detail", 
            sizeof(rule->description) - 1);
    
    rule->required_event_type = AUDIT_EVENT_DATA_ACCESS; // Primary focus
    rule->min_severity = AUDIT_SEVERITY_INFO;
    rule->retention_period_days = 365; // 1 year
    
    rule->compliance_checker = soc2_logging_checker;
    rule->is_mandatory = true;
    rule->is_enabled = true;
    
    strncpy(rule->remediation_steps, 
            "Ensure all data access events are logged with user, timestamp, and resource details",
            sizeof(rule->remediation_steps) - 1);
    
    return rule;
}

// GDPR data access logging rule
static bool gdpr_data_access_checker(const AuditEvent* event, void* context) {
    (void)context;
    
    // GDPR requires detailed logging of personal data access
    return (event->event_type == AUDIT_EVENT_DATA_ACCESS ||
            event->event_type == AUDIT_EVENT_DATA_MODIFICATION) &&
           strstr(event->event_description, "personal_data") != NULL;
}

ComplianceRule* create_gdpr_data_access_rule(void) {
    ComplianceRule* rule = xcalloc(1, sizeof(ComplianceRule));
    if (!rule) return NULL;
    
    strncpy(rule->rule_id, "GDPR-DATA-001", sizeof(rule->rule_id) - 1);
    strncpy(rule->rule_name, "GDPR Personal Data Access Logging", sizeof(rule->rule_name) - 1);
    strncpy(rule->framework, "GDPR", sizeof(rule->framework) - 1);
    strncpy(rule->description, "All access to personal data must be logged and auditable", 
            sizeof(rule->description) - 1);
    
    rule->required_event_type = AUDIT_EVENT_DATA_ACCESS;
    rule->min_severity = AUDIT_SEVERITY_INFO;
    rule->retention_period_days = 2555; // 7 years (GDPR requirement)
    
    rule->compliance_checker = gdpr_data_access_checker;
    rule->is_mandatory = true;
    rule->is_enabled = true;
    
    strncpy(rule->remediation_steps, 
            "Log all personal data access with lawful basis, purpose, and data subject information",
            sizeof(rule->remediation_steps) - 1);
    
    return rule;
}

// ISO 27001 access control rule
static bool iso27001_access_control_checker(const AuditEvent* event, void* context) {
    (void)context;
    
    // ISO 27001 requires logging of access control events
    return (event->event_type == AUDIT_EVENT_AUTHENTICATION_SUCCESS ||
            event->event_type == AUDIT_EVENT_AUTHENTICATION_FAILURE ||
            event->event_type == AUDIT_EVENT_AUTHORIZATION_SUCCESS ||
            event->event_type == AUDIT_EVENT_AUTHORIZATION_FAILURE ||
            event->event_type == AUDIT_EVENT_CAPABILITY_GRANT ||
            event->event_type == AUDIT_EVENT_CAPABILITY_DENY);
}

ComplianceRule* create_iso27001_access_control_rule(void) {
    ComplianceRule* rule = xcalloc(1, sizeof(ComplianceRule));
    if (!rule) return NULL;
    
    strncpy(rule->rule_id, "ISO27001-AC-001", sizeof(rule->rule_id) - 1);
    strncpy(rule->rule_name, "ISO 27001 Access Control Logging", sizeof(rule->rule_name) - 1);
    strncpy(rule->framework, "ISO27001", sizeof(rule->framework) - 1);
    strncpy(rule->description, "All access control decisions must be logged and monitored", 
            sizeof(rule->description) - 1);
    
    rule->required_event_type = AUDIT_EVENT_AUTHENTICATION_SUCCESS;
    rule->min_severity = AUDIT_SEVERITY_INFO;
    rule->retention_period_days = 1095; // 3 years
    
    rule->compliance_checker = iso27001_access_control_checker;
    rule->is_mandatory = true;
    rule->is_enabled = true;
    
    strncpy(rule->remediation_steps, 
            "Implement comprehensive access control logging with regular review and monitoring",
            sizeof(rule->remediation_steps) - 1);
    
    return rule;
}

// HIPAA audit rule
static bool hipaa_audit_checker(const AuditEvent* event, void* context) {
    (void)context;
    
    // HIPAA requires logging of PHI access
    return (event->event_type == AUDIT_EVENT_DATA_ACCESS ||
            event->event_type == AUDIT_EVENT_DATA_MODIFICATION) &&
           (strstr(event->event_description, "PHI") != NULL ||
            strstr(event->event_description, "medical") != NULL ||
            strstr(event->event_description, "health") != NULL);
}

ComplianceRule* create_hipaa_audit_rule(void) {
    ComplianceRule* rule = xcalloc(1, sizeof(ComplianceRule));
    if (!rule) return NULL;
    
    strncpy(rule->rule_id, "HIPAA-AUDIT-001", sizeof(rule->rule_id) - 1);
    strncpy(rule->rule_name, "HIPAA PHI Access Audit", sizeof(rule->rule_name) - 1);
    strncpy(rule->framework, "HIPAA", sizeof(rule->framework) - 1);
    strncpy(rule->description, "All access to Protected Health Information must be audited", 
            sizeof(rule->description) - 1);
    
    rule->required_event_type = AUDIT_EVENT_DATA_ACCESS;
    rule->min_severity = AUDIT_SEVERITY_INFO;
    rule->retention_period_days = 2190; // 6 years (HIPAA requirement)
    
    rule->compliance_checker = hipaa_audit_checker;
    rule->is_mandatory = true;
    rule->is_enabled = true;
    
    strncpy(rule->remediation_steps, 
            "Ensure all PHI access is logged with user identity, timestamp, and access purpose",
            sizeof(rule->remediation_steps) - 1);
    
    return rule;
}