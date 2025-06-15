#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "include/security_auditing.h"
#include "include/security_framework.h"

void test_audit_event_creation() {
    printf("Testing audit event creation...\n");
    
    AuditEvent* event = audit_event_create(AUDIT_EVENT_SECURITY_VIOLATION, 
                                          AUDIT_SEVERITY_CRITICAL, 
                                          "Test security violation");
    assert(event != NULL);
    assert(event->event_type == AUDIT_EVENT_SECURITY_VIOLATION);
    assert(event->severity == AUDIT_SEVERITY_CRITICAL);
    assert(strcmp(event->event_title, "Test security violation") == 0);
    assert(event->event_id != 0);
    assert(event->timestamp_ns != 0);
    assert(event->risk_level == RISK_LOW);
    
    audit_event_destroy(event);
    printf("✓ Audit event creation test passed\n");
}

void test_audit_event_details() {
    printf("Testing audit event details...\n");
    
    AuditEvent* event = audit_event_create(AUDIT_EVENT_DATA_ACCESS, 
                                          AUDIT_SEVERITY_INFO, 
                                          "Data access event");
    assert(event != NULL);
    
    // Set details
    Result_void_ptr result = audit_event_set_details(event, 
                                                    "User accessed sensitive data", 
                                                    "data_protection");
    assert(!result.is_error);
    assert(strcmp(event->event_description, "User accessed sensitive data") == 0);
    assert(strcmp(event->event_category, "data_protection") == 0);
    
    // Set security context
    result = audit_event_set_security_context(event, CAP_READ_FILE, TAINT_USER_INPUT);
    assert(!result.is_error);
    assert(event->involved_capabilities == CAP_READ_FILE);
    assert(event->involved_taint_level == TAINT_USER_INPUT);
    
    // Set source info
    result = audit_event_set_source_info(event, "test_module", "test_function", "test.c", 42);
    assert(!result.is_error);
    assert(strcmp(event->source_module, "test_module") == 0);
    assert(strcmp(event->source_function, "test_function") == 0);
    assert(strcmp(event->source_file, "test.c") == 0);
    assert(event->source_line == 42);
    
    // Set correlation
    result = audit_event_add_correlation(event, 12345, 67890);
    assert(!result.is_error);
    assert(event->correlation_id == 12345);
    assert(event->parent_event_id == 67890);
    
    audit_event_destroy(event);
    printf("✓ Audit event details test passed\n");
}

void test_audit_logger_creation() {
    printf("Testing audit logger creation...\n");
    
    AuditLogger* logger = audit_logger_create();
    assert(logger != NULL);
    assert(logger->memory_config.event_buffer != NULL);
    assert(logger->memory_config.buffer_capacity == 10000);
    assert(logger->memory_config.buffer_size == 0);
    assert(logger->policy.async_logging == true);
    assert(logger->policy.min_log_level == AUDIT_SEVERITY_INFO);
    
    audit_logger_destroy(logger);
    printf("✓ Audit logger creation test passed\n");
}

void test_audit_analyzer_creation() {
    printf("Testing audit analyzer creation...\n");
    
    AuditAnalyzer* analyzer = audit_analyzer_create();
    assert(analyzer != NULL);
    assert(analyzer->anomaly_detector.baseline_event_rate == 100.0);
    assert(analyzer->anomaly_detector.anomaly_threshold == 2.0);
    assert(analyzer->correlator.enable_correlation == true);
    assert(analyzer->correlator.correlation_window_ms == 5 * 60 * 1000);
    
    audit_analyzer_destroy(analyzer);
    printf("✓ Audit analyzer creation test passed\n");
}

void test_compliance_checker_creation() {
    printf("Testing compliance checker creation...\n");
    
    ComplianceChecker* checker = compliance_checker_create();
    assert(checker != NULL);
    assert(checker->frameworks != NULL);
    assert(checker->status.overall_compliance_score == 100.0);
    assert(checker->status.is_compliant == true);
    
    compliance_checker_destroy(checker);
    printf("✓ Compliance checker creation test passed\n");
}

void test_threat_detector_creation() {
    printf("Testing threat detector creation...\n");
    
    ThreatDetector* detector = threat_detector_create();
    assert(detector != NULL);
    assert(detector->config.enable_real_time_detection == true);
    assert(detector->config.threat_threshold == 7.0);
    assert(detector->active_threats != NULL);
    assert(detector->threat_capacity == 100);
    assert(detector->ml_model.learning_rate == 0.01);
    
    threat_detector_destroy(detector);
    printf("✓ Threat detector creation test passed\n");
}

void test_security_audit_system_creation() {
    printf("Testing security audit system creation...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    assert(sec_ctx != NULL);
    
    SecurityAuditSystem* audit_system = security_audit_system_create(sec_ctx);
    assert(audit_system != NULL);
    assert(audit_system->security_context == sec_ctx);
    assert(audit_system->logger != NULL);
    assert(audit_system->analyzer != NULL);
    assert(audit_system->compliance_checker != NULL);
    assert(audit_system->threat_detector != NULL);
    assert(audit_system->event_processing.event_queue != NULL);
    assert(audit_system->config.audit_enabled == true);
    assert(audit_system->config.real_time_processing == true);
    
    // Initialize the system
    Result_void_ptr result = security_audit_system_initialize(audit_system);
    assert(!result.is_error);
    assert(audit_system->is_initialized == true);
    
    security_audit_system_destroy(audit_system);
    security_context_destroy(sec_ctx);
    printf("✓ Security audit system creation test passed\n");
}

void test_audit_event_logging() {
    printf("Testing audit event logging...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    SecurityAuditSystem* audit_system = security_audit_system_create(sec_ctx);
    security_audit_system_initialize(audit_system);
    
    // Test simple logging
    Result_void_ptr result = security_audit_log_simple(audit_system, 
                                                       AUDIT_EVENT_USER_ACTION,
                                                       AUDIT_SEVERITY_INFO,
                                                       "User logged in");
    assert(!result.is_error);
    assert(audit_system->statistics.total_events_processed == 1);
    assert(audit_system->analyzer->statistics.total_events == 1);
    
    // Test event object logging
    AuditEvent* event = audit_event_create(AUDIT_EVENT_DATA_ACCESS, 
                                          AUDIT_SEVERITY_WARNING, 
                                          "Sensitive data accessed");
    audit_event_set_details(event, "User accessed customer data", "data_security");
    audit_event_set_security_context(event, CAP_DATABASE_READ, TAINT_DATABASE);
    
    result = security_audit_log_event(audit_system, event);
    assert(!result.is_error);
    assert(audit_system->statistics.total_events_processed == 2);
    
    security_audit_system_destroy(audit_system);
    security_context_destroy(sec_ctx);
    printf("✓ Audit event logging test passed\n");
}

void test_configuration() {
    printf("Testing audit system configuration...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_STRICT);
    SecurityAuditSystem* audit_system = security_audit_system_create(sec_ctx);
    
    // Test default configuration
    SecurityAuditConfig default_config = security_audit_config_default();
    assert(default_config.enable_real_time_processing == true);
    assert(default_config.enable_threat_detection == true);
    assert(default_config.max_events_per_second == 10000);
    assert(default_config.processing_threads == 2);
    assert(default_config.log_destinations & AUDIT_DEST_FILE);
    assert(default_config.log_destinations & AUDIT_DEST_MEMORY);
    
    // Test high security configuration
    SecurityAuditConfig high_sec_config = security_audit_config_high_security();
    assert(high_sec_config.enable_anomaly_detection == true);
    assert(high_sec_config.event_retention_days == 2555);
    assert(high_sec_config.enable_encryption == true);
    assert(high_sec_config.enable_integrity_checking == true);
    
    // Test high performance configuration
    SecurityAuditConfig high_perf_config = security_audit_config_high_performance();
    assert(high_perf_config.max_events_per_second == 100000);
    assert(high_perf_config.processing_threads == 8);
    assert(high_perf_config.log_destinations == AUDIT_DEST_MEMORY);
    
    // Apply configuration
    Result_void_ptr result = security_audit_system_configure(audit_system, default_config);
    assert(!result.is_error);
    
    security_audit_system_destroy(audit_system);
    security_context_destroy(sec_ctx);
    printf("✓ Configuration test passed\n");
}

void test_threat_patterns() {
    printf("Testing threat patterns...\n");
    
    // Test brute force pattern
    ThreatPattern* brute_force = create_brute_force_pattern();
    assert(brute_force != NULL);
    assert(strcmp(brute_force->pattern_name, "brute_force_attack") == 0);
    assert(brute_force->event_type_count == 1);
    assert(brute_force->event_types[0] == AUDIT_EVENT_AUTHENTICATION_FAILURE);
    assert(brute_force->time_window_ms == 5 * 60 * 1000);
    assert(brute_force->min_occurrences == 5);
    assert(brute_force->response_action == RESPONSE_ALERT);
    assert(brute_force->is_enabled == true);
    free(brute_force->event_types);
    free(brute_force);
    
    // Test privilege escalation pattern
    ThreatPattern* priv_esc = create_privilege_escalation_pattern();
    assert(priv_esc != NULL);
    assert(strcmp(priv_esc->pattern_name, "privilege_escalation") == 0);
    assert(priv_esc->event_type_count == 2);
    assert(priv_esc->response_action == RESPONSE_BLOCK);
    assert(priv_esc->min_risk_score == 8.0);
    free(priv_esc->event_types);
    free(priv_esc);
    
    // Test data exfiltration pattern
    ThreatPattern* data_exfil = create_data_exfiltration_pattern();
    assert(data_exfil != NULL);
    assert(strcmp(data_exfil->pattern_name, "data_exfiltration") == 0);
    assert(data_exfil->event_type_count == 3);
    assert(data_exfil->response_action == RESPONSE_ESCALATE);
    free(data_exfil->event_types);
    free(data_exfil);
    
    // Test injection attack pattern
    ThreatPattern* injection = create_injection_attack_pattern();
    assert(injection != NULL);
    assert(strcmp(injection->pattern_name, "injection_attack") == 0);
    assert(injection->response_action == RESPONSE_BLOCK);
    assert(injection->min_risk_score == 8.5);
    free(injection->event_types);
    free(injection);
    
    printf("✓ Threat patterns test passed\n");
}

void test_compliance_rules() {
    printf("Testing compliance rules...\n");
    
    // Test SOC 2 rule
    ComplianceRule* soc2_rule = create_soc2_logging_rule();
    assert(soc2_rule != NULL);
    assert(strcmp(soc2_rule->rule_id, "SOC2-LOG-001") == 0);
    assert(strcmp(soc2_rule->framework, "SOC2") == 0);
    assert(soc2_rule->retention_period_days == 365);
    assert(soc2_rule->is_mandatory == true);
    assert(soc2_rule->compliance_checker != NULL);
    free(soc2_rule);
    
    // Test GDPR rule
    ComplianceRule* gdpr_rule = create_gdpr_data_access_rule();
    assert(gdpr_rule != NULL);
    assert(strcmp(gdpr_rule->rule_id, "GDPR-DATA-001") == 0);
    assert(strcmp(gdpr_rule->framework, "GDPR") == 0);
    assert(gdpr_rule->retention_period_days == 2555); // 7 years
    free(gdpr_rule);
    
    // Test ISO 27001 rule
    ComplianceRule* iso_rule = create_iso27001_access_control_rule();
    assert(iso_rule != NULL);
    assert(strcmp(iso_rule->framework, "ISO27001") == 0);
    assert(iso_rule->retention_period_days == 1095); // 3 years
    free(iso_rule);
    
    // Test HIPAA rule
    ComplianceRule* hipaa_rule = create_hipaa_audit_rule();
    assert(hipaa_rule != NULL);
    assert(strcmp(hipaa_rule->framework, "HIPAA") == 0);
    assert(hipaa_rule->retention_period_days == 2190); // 6 years
    free(hipaa_rule);
    
    printf("✓ Compliance rules test passed\n");
}

void test_integration_with_security_systems() {
    printf("Testing integration with security systems...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    SecurityAuditSystem* audit_system = security_audit_system_create(sec_ctx);
    security_audit_system_initialize(audit_system);
    
    // Create and integrate taint analyzer
    TaintAnalyzer* taint_analyzer = taint_analyzer_create(sec_ctx);
    Result_void_ptr result = security_audit_integrate_taint_analysis(audit_system, taint_analyzer);
    assert(!result.is_error);
    assert(audit_system->taint_analyzer == taint_analyzer);
    
    // Create and integrate capability system
    CapabilitySystem* cap_system = capability_system_create(sec_ctx);
    result = security_audit_integrate_capability_system(audit_system, cap_system);
    assert(!result.is_error);
    assert(audit_system->capability_system == cap_system);
    
    // Test capability event logging
    result = security_audit_log_capability_event(audit_system, CAP_READ_FILE, "test_entity", true);
    assert(!result.is_error);
    assert(audit_system->statistics.total_events_processed >= 1);
    
    taint_analyzer_destroy(taint_analyzer);
    capability_system_destroy(cap_system);
    security_audit_system_destroy(audit_system);
    security_context_destroy(sec_ctx);
    printf("✓ Integration test passed\n");
}

void test_audit_statistics() {
    printf("Testing audit statistics...\n");
    
    SecurityContext* sec_ctx = security_context_create(SECURITY_POLICY_MODERATE);
    SecurityAuditSystem* audit_system = security_audit_system_create(sec_ctx);
    security_audit_system_initialize(audit_system);
    
    // Log multiple events of different types and severities
    security_audit_log_simple(audit_system, AUDIT_EVENT_USER_ACTION, AUDIT_SEVERITY_INFO, "User login");
    security_audit_log_simple(audit_system, AUDIT_EVENT_DATA_ACCESS, AUDIT_SEVERITY_WARNING, "Data access");
    security_audit_log_simple(audit_system, AUDIT_EVENT_SECURITY_VIOLATION, AUDIT_SEVERITY_CRITICAL, "Security violation");
    
    // Check statistics
    assert(audit_system->statistics.total_events_processed == 3);
    assert(audit_system->analyzer->statistics.total_events == 3);
    assert(audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_INFO] >= 1);
    assert(audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_WARNING] >= 1);
    assert(audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_CRITICAL] >= 1);
    
    printf("  - Total events processed: %llu\n", audit_system->statistics.total_events_processed);
    printf("  - Info events: %llu\n", audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_INFO]);
    printf("  - Warning events: %llu\n", audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_WARNING]);
    printf("  - Critical events: %llu\n", audit_system->analyzer->statistics.events_by_severity[AUDIT_SEVERITY_CRITICAL]);
    
    security_audit_system_destroy(audit_system);
    security_context_destroy(sec_ctx);
    printf("✓ Audit statistics test passed\n");
}

int main() {
    printf("=== Security Auditing System Tests ===\n\n");
    
    test_audit_event_creation();
    test_audit_event_details();
    test_audit_logger_creation();
    test_audit_analyzer_creation();
    test_compliance_checker_creation();
    test_threat_detector_creation();
    test_security_audit_system_creation();
    test_audit_event_logging();
    test_configuration();
    test_threat_patterns();
    test_compliance_rules();
    test_integration_with_security_systems();
    test_audit_statistics();
    
    printf("\n=== All Security Auditing Tests Passed! ===\n");
    return 0;
}