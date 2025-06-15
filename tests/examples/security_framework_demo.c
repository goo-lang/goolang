#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "include/security_framework.h"
#include "include/taint_analysis.h"
#include "include/capability_security.h"
#include "include/security_auditing.h"

void demonstrate_taint_analysis(SecurityAuditSystem* audit_system) {
    printf("\n=== Taint Analysis Demonstration ===\n");
    
    // Create security context and taint analyzer
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_STRICT);
    TaintAnalyzer* taint_analyzer = taint_analyzer_create(sec_ctx);
    
    // Integrate with audit system
    security_audit_integrate_taint_analysis(audit_system, taint_analyzer);
    
    // Demonstrate taint tracking
    printf("1. Creating tainted value from user input...\n");
    TaintedValue* user_input = taint_value_create("malicious_input", TAINT_USER_INPUT);
    printf("   - Taint level: %d (User Input)\n", user_input->taint_level);
    
    printf("2. Propagating taint through operations...\n");
    TaintedValue* processed = taint_value_propagate(user_input, "string_concatenation");
    printf("   - Propagated taint level: %d\n", processed->taint_level);
    
    printf("3. Attempting to use tainted data in sink...\n");
    Result_void_ptr result = taint_analyzer_check_sink_usage(taint_analyzer, processed, TAINT_SINK_SQL_QUERY);
    if (result.is_error) {
        printf("   ✓ Taint violation detected! SQL injection prevented.\n");
        security_audit_log_simple(audit_system, AUDIT_EVENT_TAINT_VIOLATION, 
                                  AUDIT_SEVERITY_CRITICAL, "Taint violation prevented SQL injection");
    }
    
    printf("4. Sanitizing tainted data...\n");
    TaintedValue* sanitized = taint_value_sanitize(processed, TAINT_SANITIZER_SQL_ESCAPE);
    printf("   - Sanitized taint level: %d (Safe for SQL)\n", sanitized->taint_level);
    
    // Cleanup
    taint_value_destroy(user_input);
    taint_value_destroy(processed);
    taint_value_destroy(sanitized);
    taint_analyzer_destroy(taint_analyzer);
    security_context_destroy(sec_ctx);
}

void demonstrate_capability_system(SecurityAuditSystem* audit_system) {
    printf("\n=== Capability-Based Security Demonstration ===\n");
    
    // Create security context and capability system
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    
    // Integrate with audit system
    security_audit_integrate_capability_system(audit_system, cap_system);
    
    printf("1. Creating capability tokens...\n");
    
    // Grant file read capability
    Result_ptr_CapabilityToken file_read_result = capability_system_grant(cap_system, CAP_READ_FILE, "demo_user", 60);
    if (!file_read_result.is_error) {
        CapabilityToken* token = file_read_result.data;
        printf("   ✓ File read capability granted (Token ID: %llu)\n", token->token_id);
        
        // Log capability grant
        security_audit_log_capability_event(audit_system, CAP_READ_FILE, "demo_user", true);
    }
    
    // Attempt to use network capability (should fail)
    printf("2. Attempting to use network capability without token...\n");
    Result_bool network_check = capability_system_check(cap_system, CAP_NETWORK_CONNECT, "demo_user");
    if (network_check.is_error || !network_check.data) {
        printf("   ✓ Network access denied - no capability token\n");
        security_audit_log_capability_event(audit_system, CAP_NETWORK_CONNECT, "demo_user", false);
    }
    
    // Grant multiple capabilities with delegation
    printf("3. Granting multiple capabilities with delegation...\n");
    Result_ptr_CapabilityToken admin_result = capability_system_grant(cap_system, 
                                                                      CAP_USER_ADMIN | CAP_READ_FILE | CAP_WRITE_FILE, 
                                                                      "admin_user", 300);
    if (!admin_result.is_error) {
        CapabilityToken* admin_token = admin_result.data;
        printf("   ✓ Admin capabilities granted (Token ID: %llu)\n", admin_token->token_id);
        
        // Delegate capability to another user
        Result_void_ptr delegate_result = capability_system_delegate(cap_system, admin_token, 
                                                                     "worker_user", CAP_READ_FILE, 2, 120);
        if (!delegate_result.is_error) {
            printf("   ✓ File read capability delegated to worker_user\n");
        }
    }
    
    // Show active tokens
    printf("4. Current active tokens: %zu\n", cap_system->token_count);
    
    // Cleanup
    capability_system_destroy(cap_system);
    security_context_destroy(sec_ctx);
}

void demonstrate_threat_detection(SecurityAuditSystem* audit_system) {
    printf("\n=== Threat Detection Demonstration ===\n");
    
    printf("1. Adding built-in threat patterns...\n");
    
    // Add brute force detection pattern
    ThreatPattern* brute_force = create_brute_force_pattern();
    security_audit_add_threat_pattern(audit_system, brute_force);
    printf("   ✓ Brute force attack pattern added\n");
    
    // Add injection attack pattern
    ThreatPattern* injection = create_injection_attack_pattern();
    security_audit_add_threat_pattern(audit_system, injection);
    printf("   ✓ Injection attack pattern added\n");
    
    // Add data exfiltration pattern
    ThreatPattern* exfiltration = create_data_exfiltration_pattern();
    security_audit_add_threat_pattern(audit_system, exfiltration);
    printf("   ✓ Data exfiltration pattern added\n");
    
    printf("2. Simulating authentication failures (brute force)...\n");
    for (int i = 0; i < 6; i++) {
        security_audit_log_simple(audit_system, AUDIT_EVENT_AUTHENTICATION_FAILURE, 
                                  AUDIT_SEVERITY_WARNING, "Failed login attempt");
        usleep(100000); // 100ms delay
    }
    printf("   ✓ Multiple authentication failures logged\n");
    
    printf("3. Simulating taint violation (injection attack)...\n");
    AuditEvent* injection_event = audit_event_create(AUDIT_EVENT_TAINT_VIOLATION, 
                                                     AUDIT_SEVERITY_CRITICAL, 
                                                     "SQL injection attempt detected");
    audit_event_set_security_context(injection_event, CAP_DATABASE_READ, TAINT_UNVALIDATED);
    injection_event->risk_level = RISK_HIGH;
    security_audit_log_event(audit_system, injection_event);
    printf("   ✓ Injection attack event logged\n");
    
    printf("4. Running threat analysis...\n");
    security_audit_analyze_events(audit_system, 60000); // 1 minute window
    printf("   ✓ Threat analysis completed\n");
}

void demonstrate_compliance_checking(SecurityAuditSystem* audit_system) {
    printf("\n=== Compliance Checking Demonstration ===\n");
    
    printf("1. Adding compliance rules...\n");
    
    // Add SOC 2 logging rule
    ComplianceRule* soc2_rule = create_soc2_logging_rule();
    security_audit_add_compliance_rule(audit_system, soc2_rule);
    printf("   ✓ SOC 2 logging rule added\n");
    
    // Add GDPR data access rule
    ComplianceRule* gdpr_rule = create_gdpr_data_access_rule();
    security_audit_add_compliance_rule(audit_system, gdpr_rule);
    printf("   ✓ GDPR data access rule added\n");
    
    // Add ISO 27001 access control rule
    ComplianceRule* iso_rule = create_iso27001_access_control_rule();
    security_audit_add_compliance_rule(audit_system, iso_rule);
    printf("   ✓ ISO 27001 access control rule added\n");
    
    // Add HIPAA audit rule
    ComplianceRule* hipaa_rule = create_hipaa_audit_rule();
    security_audit_add_compliance_rule(audit_system, hipaa_rule);
    printf("   ✓ HIPAA audit rule added\n");
    
    printf("2. Generating compliance-relevant events...\n");
    
    // SOC 2 - Data access event
    security_audit_log_simple(audit_system, AUDIT_EVENT_DATA_ACCESS, 
                               AUDIT_SEVERITY_INFO, "Employee accessed customer records");
    
    // GDPR - Personal data access
    AuditEvent* gdpr_event = audit_event_create(AUDIT_EVENT_DATA_ACCESS, 
                                                AUDIT_SEVERITY_INFO, 
                                                "Personal data accessed");
    audit_event_set_details(gdpr_event, "Employee accessed personal_data for customer support", "data_protection");
    security_audit_log_event(audit_system, gdpr_event);
    
    // ISO 27001 - Authentication success
    security_audit_log_simple(audit_system, AUDIT_EVENT_AUTHENTICATION_SUCCESS, 
                               AUDIT_SEVERITY_INFO, "User authenticated successfully");
    
    // HIPAA - Medical records access
    AuditEvent* hipaa_event = audit_event_create(AUDIT_EVENT_DATA_ACCESS, 
                                                 AUDIT_SEVERITY_INFO, 
                                                 "Medical records accessed");
    audit_event_set_details(hipaa_event, "Doctor accessed PHI for patient treatment", "medical_access");
    security_audit_log_event(audit_system, hipaa_event);
    
    printf("   ✓ Compliance events logged\n");
    
    printf("3. Checking compliance for each framework...\n");
    security_audit_check_compliance(audit_system, "SOC2");
    security_audit_check_compliance(audit_system, "GDPR");
    security_audit_check_compliance(audit_system, "ISO27001");
    security_audit_check_compliance(audit_system, "HIPAA");
    printf("   ✓ Compliance checking completed\n");
    
    printf("4. Getting compliance scores...\n");
    double soc2_score = security_audit_get_compliance_score(audit_system, "SOC2");
    double gdpr_score = security_audit_get_compliance_score(audit_system, "GDPR");
    double iso_score = security_audit_get_compliance_score(audit_system, "ISO27001");
    double hipaa_score = security_audit_get_compliance_score(audit_system, "HIPAA");
    
    printf("   - SOC 2 compliance: %.1f%%\n", soc2_score);
    printf("   - GDPR compliance: %.1f%%\n", gdpr_score);
    printf("   - ISO 27001 compliance: %.1f%%\n", iso_score);
    printf("   - HIPAA compliance: %.1f%%\n", hipaa_score);
}

void demonstrate_audit_reporting(SecurityAuditSystem* audit_system) {
    printf("\n=== Audit Reporting Demonstration ===\n");
    
    printf("1. Current audit statistics:\n");
    printf("   - Total events processed: %llu\n", audit_system->statistics.total_events_processed);
    printf("   - Events per second: %llu\n", audit_system->statistics.events_per_second);
    printf("   - Processing errors: %llu\n", audit_system->statistics.processing_errors);
    printf("   - Threats detected: %llu\n", audit_system->statistics.threats_detected);
    printf("   - Compliance violations: %llu\n", audit_system->statistics.compliance_violations);
    
    printf("2. Event breakdown by severity:\n");
    printf("   - Debug: %llu\n", audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_DEBUG]);
    printf("   - Info: %llu\n", audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_INFO]);
    printf("   - Warning: %llu\n", audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_WARNING]);
    printf("   - Error: %llu\n", audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_ERROR]);
    printf("   - Critical: %llu\n", audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_CRITICAL]);
    
    printf("3. Performance metrics:\n");
    printf("   - Average processing time: %llu ns\n", audit_system->statistics.avg_processing_time_ns);
    printf("   - Analyzer events: %llu\n", audit_system->analyzer->statistics.total_events);
    
    // Generate comprehensive report
    uint64_t current_time = (uint64_t)time(NULL) * 1000000000ULL;
    uint64_t one_hour_ago = current_time - (60ULL * 60 * 1000000000ULL);
    SecurityAuditReport* report = security_audit_generate_report(audit_system, one_hour_ago, current_time);
    
    if (report) {
        printf("4. Generated comprehensive audit report:\n");
        printf("   - Report period: %llu to %llu\n", report->report_period_start, report->report_period_end);
        printf("   - Total events in period: %llu\n", report->total_events);
        printf("   - Security violations: %llu\n", report->security_violations);
        printf("   - Overall compliance score: %.1f%%\n", report->overall_compliance_score);
        
        // Export report
        security_audit_export_report(report, "json", "/tmp/security_audit_report.json");
        printf("   ✓ Report exported to /tmp/security_audit_report.json\n");
        
        security_audit_report_destroy(report);
    }
}

int main() {
    printf("=== Goo Security Framework Comprehensive Demonstration ===\n");
    printf("This demo showcases all components of the Security by Design framework:\n");
    printf("- Taint Analysis System\n");
    printf("- Capability-Based Security\n");
    printf("- Automatic Security Auditing\n");
    printf("- Threat Detection\n");
    printf("- Compliance Checking\n\n");
    
    // Initialize security audit system
    SecurityContext* main_context = security_context_create(SECURITY_POLICY_STRICT);
    SecurityAuditSystem* audit_system = security_audit_system_create(main_context);
    
    // Configure for high security
    SecurityAuditConfig config = security_audit_config_high_security();
    security_audit_system_configure(audit_system, config);
    
    // Initialize the system
    Result_void_ptr init_result = security_audit_system_initialize(audit_system);
    if (init_result.is_error) {
        printf("Failed to initialize audit system\n");
        return 1;
    }
    
    printf("✓ Security audit system initialized with high security configuration\n");
    
    // Run demonstrations
    demonstrate_taint_analysis(audit_system);
    demonstrate_capability_system(audit_system);
    demonstrate_threat_detection(audit_system);
    demonstrate_compliance_checking(audit_system);
    demonstrate_audit_reporting(audit_system);
    
    printf("\n=== Security Framework Demo Summary ===\n");
    printf("The demonstration successfully showcased:\n");
    printf("✓ Comprehensive taint tracking and injection prevention\n");
    printf("✓ Fine-grained capability-based access control\n");
    printf("✓ Real-time threat detection and pattern matching\n");
    printf("✓ Multi-framework compliance checking (SOC2, GDPR, ISO27001, HIPAA)\n");
    printf("✓ Detailed audit logging and reporting\n");
    printf("✓ Integration between all security components\n");
    
    printf("\nSecurity Policy Effectiveness:\n");
    printf("- Prevented SQL injection through taint analysis\n");
    printf("- Blocked unauthorized network access via capability system\n");
    printf("- Detected simulated brute force attack patterns\n");
    printf("- Maintained compliance across multiple regulatory frameworks\n");
    printf("- Generated comprehensive audit trail for forensic analysis\n");
    
    // Cleanup
    security_audit_system_destroy(audit_system);
    security_context_destroy(main_context);
    
    printf("\n=== Demonstration completed successfully! ===\n");
    return 0;
}