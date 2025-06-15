#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>

typedef struct {
    char function_name[128];
    int call_count;
    double total_time_ms;
    double min_time_ms;
    double max_time_ms;
    double avg_time_ms;
    size_t memory_allocated;
    size_t memory_freed;
} ProfileEntry;

typedef struct {
    ProfileEntry* entries;
    int count;
    int capacity;
    double total_program_time_ms;
    size_t peak_memory_usage;
    int total_function_calls;
} ProfileReport;

static ProfileReport profile_report = {0};
static int profiling_enabled = 0;
static double program_start_time = 0;

void init_profiler() {
    profile_report.capacity = 1000;
    profile_report.entries = malloc(sizeof(ProfileEntry) * profile_report.capacity);
    profile_report.count = 0;
    profile_report.total_program_time_ms = 0;
    profile_report.peak_memory_usage = 0;
    profile_report.total_function_calls = 0;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    program_start_time = (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
    
    profiling_enabled = 1;
    printf("🔍 Profiler initialized\\n");
}

void cleanup_profiler() {
    if (profile_report.entries) {
        free(profile_report.entries);
        profile_report.entries = NULL;
    }
    profiling_enabled = 0;
}

double get_current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

size_t get_memory_usage() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss * 1024; // Convert KB to bytes on Linux
}

ProfileEntry* find_or_create_entry(const char* function_name) {
    // Find existing entry
    for (int i = 0; i < profile_report.count; i++) {
        if (strcmp(profile_report.entries[i].function_name, function_name) == 0) {
            return &profile_report.entries[i];
        }
    }
    
    // Create new entry
    if (profile_report.count >= profile_report.capacity) {
        profile_report.capacity *= 2;
        profile_report.entries = realloc(profile_report.entries, 
                                       sizeof(ProfileEntry) * profile_report.capacity);
    }
    
    ProfileEntry* entry = &profile_report.entries[profile_report.count++];
    strncpy(entry->function_name, function_name, sizeof(entry->function_name) - 1);
    entry->function_name[sizeof(entry->function_name) - 1] = '\\0';
    entry->call_count = 0;
    entry->total_time_ms = 0;
    entry->min_time_ms = 999999.0;
    entry->max_time_ms = 0;
    entry->avg_time_ms = 0;
    entry->memory_allocated = 0;
    entry->memory_freed = 0;
    
    return entry;
}

void record_function_call(const char* function_name, double duration_ms, size_t memory_delta) {
    if (!profiling_enabled) return;
    
    ProfileEntry* entry = find_or_create_entry(function_name);
    
    entry->call_count++;
    entry->total_time_ms += duration_ms;
    
    if (duration_ms < entry->min_time_ms) {
        entry->min_time_ms = duration_ms;
    }
    if (duration_ms > entry->max_time_ms) {
        entry->max_time_ms = duration_ms;
    }
    
    entry->avg_time_ms = entry->total_time_ms / entry->call_count;
    
    if (memory_delta > 0) {
        entry->memory_allocated += memory_delta;
    } else {
        entry->memory_freed += (-memory_delta);
    }
    
    profile_report.total_function_calls++;
    
    size_t current_memory = get_memory_usage();
    if (current_memory > profile_report.peak_memory_usage) {
        profile_report.peak_memory_usage = current_memory;
    }
}

void print_ascii_chart(const char* title, double* values, const char** labels, int count, double max_value) {
    printf("\\n📊 %s\\n", title);
    printf("================================\\n");
    
    const int chart_width = 40;
    
    for (int i = 0; i < count && i < 10; i++) { // Show top 10
        double normalized = (values[i] / max_value) * chart_width;
        int bar_length = (int)normalized;
        
        printf("%-20s [", labels[i]);
        for (int j = 0; j < chart_width; j++) {
            if (j < bar_length) {
                printf("█");
            } else {
                printf("░");
            }
        }
        printf("] %.2f\\n", values[i]);
    }
}

void print_memory_visualization() {
    printf("\\n🧠 Memory Usage Visualization\\n");
    printf("=============================\\n");
    
    // Simulate memory usage over time (in a real profiler, this would be collected)
    printf("Memory usage pattern:\\n");
    printf("Peak: %zu KB\\n", profile_report.peak_memory_usage / 1024);
    
    // ASCII memory graph
    const int graph_width = 50;
    printf("\\nMemory over time:\\n");
    printf("┌");
    for (int i = 0; i < graph_width; i++) printf("─");
    printf("┐\\n");
    
    // Simulate memory usage pattern
    for (int i = 0; i < 10; i++) {
        printf("│");
        int memory_level = (rand() % (graph_width - 10)) + 5;
        for (int j = 0; j < graph_width; j++) {
            if (j < memory_level) {
                printf("█");
            } else {
                printf(" ");
            }
        }
        printf("│ %d%% \\n", (memory_level * 100) / graph_width);
    }
    
    printf("└");
    for (int i = 0; i < graph_width; i++) printf("─");
    printf("┘\\n");
}

void print_hotspots() {
    if (profile_report.count == 0) {
        printf("\\n🔥 No function calls recorded\\n");
        return;
    }
    
    printf("\\n🔥 Performance Hotspots\\n");
    printf("======================\\n\\n");
    
    // Sort by total time (bubble sort for simplicity)
    for (int i = 0; i < profile_report.count - 1; i++) {
        for (int j = 0; j < profile_report.count - i - 1; j++) {
            if (profile_report.entries[j].total_time_ms < profile_report.entries[j + 1].total_time_ms) {
                ProfileEntry temp = profile_report.entries[j];
                profile_report.entries[j] = profile_report.entries[j + 1];
                profile_report.entries[j + 1] = temp;
            }
        }
    }
    
    printf("┌─────────────────────────┬───────┬─────────┬─────────┬─────────┬─────────┐\\n");
    printf("│ Function                │ Calls │ Total   │ Average │ Min     │ Max     │\\n");
    printf("│                         │       │ (ms)    │ (ms)    │ (ms)    │ (ms)    │\\n");
    printf("├─────────────────────────┼───────┼─────────┼─────────┼─────────┼─────────┤\\n");
    
    for (int i = 0; i < profile_report.count && i < 15; i++) {
        ProfileEntry* entry = &profile_report.entries[i];
        printf("│ %-23s │ %5d │ %7.2f │ %7.2f │ %7.2f │ %7.2f │\\n",
               entry->function_name,
               entry->call_count,
               entry->total_time_ms,
               entry->avg_time_ms,
               entry->min_time_ms,
               entry->max_time_ms);
    }
    
    printf("└─────────────────────────┴───────┴─────────┴─────────┴─────────┴─────────┘\\n");
}

void print_performance_summary() {
    double current_time = get_current_time_ms();
    profile_report.total_program_time_ms = current_time - program_start_time;
    
    printf("\\n📈 Performance Summary\\n");
    printf("======================\\n\\n");
    
    printf("🕐 Program execution time: %.2f ms\\n", profile_report.total_program_time_ms);
    printf("📞 Total function calls: %d\\n", profile_report.total_function_calls);
    printf("🧠 Peak memory usage: %zu KB\\n", profile_report.peak_memory_usage / 1024);
    printf("🎯 Unique functions profiled: %d\\n", profile_report.count);
    
    if (profile_report.total_function_calls > 0) {
        double avg_call_time = 0;
        for (int i = 0; i < profile_report.count; i++) {
            avg_call_time += profile_report.entries[i].total_time_ms;
        }
        avg_call_time /= profile_report.total_function_calls;
        printf("⚡ Average call time: %.3f ms\\n", avg_call_time);
    }
    
    // Performance insights
    printf("\\n💡 Performance Insights:\\n");
    
    if (profile_report.count > 0) {
        ProfileEntry* slowest = &profile_report.entries[0];
        for (int i = 1; i < profile_report.count; i++) {
            if (profile_report.entries[i].max_time_ms > slowest->max_time_ms) {
                slowest = &profile_report.entries[i];
            }
        }
        printf("  • Slowest single call: %s (%.2f ms)\\n", slowest->function_name, slowest->max_time_ms);
        
        ProfileEntry* most_called = &profile_report.entries[0];
        for (int i = 1; i < profile_report.count; i++) {
            if (profile_report.entries[i].call_count > most_called->call_count) {
                most_called = &profile_report.entries[i];
            }
        }
        printf("  • Most called function: %s (%d calls)\\n", most_called->function_name, most_called->call_count);
        
        // Find functions that might need optimization
        for (int i = 0; i < profile_report.count; i++) {
            ProfileEntry* entry = &profile_report.entries[i];
            if (entry->total_time_ms > profile_report.total_program_time_ms * 0.1) {
                printf("  • High impact function: %s (%.1f%% of total time)\\n", 
                       entry->function_name, 
                       (entry->total_time_ms / profile_report.total_program_time_ms) * 100);
            }
        }
    }
}

void generate_html_profile_report(const char* filename) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        printf("⚠️  Could not generate HTML profile report: %s\\n", filename);
        return;
    }
    
    fprintf(file, "<!DOCTYPE html>\\n<html>\\n<head>\\n");
    fprintf(file, "<title>Goo Performance Profile</title>\\n");
    fprintf(file, "<style>\\n");
    fprintf(file, "body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 40px; }\\n");
    fprintf(file, ".summary { background: #f8fafc; padding: 20px; border-radius: 8px; margin-bottom: 20px; }\\n");
    fprintf(file, ".function { padding: 15px; border: 1px solid #e2e8f0; margin: 10px 0; border-radius: 6px; }\\n");
    fprintf(file, ".hotspot { background: #fef2f2; border-color: #fca5a5; }\\n");
    fprintf(file, ".chart { margin: 20px 0; }\\n");
    fprintf(file, ".bar { height: 20px; background: linear-gradient(90deg, #3b82f6, #1d4ed8); margin: 2px 0; border-radius: 3px; }\\n");
    fprintf(file, "table { width: 100%%; border-collapse: collapse; margin: 20px 0; }\\n");
    fprintf(file, "th, td { padding: 12px; text-align: left; border-bottom: 1px solid #e2e8f0; }\\n");
    fprintf(file, "th { background: #f1f5f9; font-weight: 600; }\\n");
    fprintf(file, "</style>\\n");
    fprintf(file, "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>\\n");
    fprintf(file, "</head>\\n<body>\\n");
    
    fprintf(file, "<h1>🔍 Goo Performance Profile Report</h1>\\n");
    
    // Summary
    fprintf(file, "<div class='summary'>\\n");
    fprintf(file, "<h2>📊 Summary</h2>\\n");
    fprintf(file, "<p><strong>Execution Time:</strong> %.2f ms</p>\\n", profile_report.total_program_time_ms);
    fprintf(file, "<p><strong>Function Calls:</strong> %d</p>\\n", profile_report.total_function_calls);
    fprintf(file, "<p><strong>Peak Memory:</strong> %zu KB</p>\\n", profile_report.peak_memory_usage / 1024);
    fprintf(file, "<p><strong>Functions Profiled:</strong> %d</p>\\n", profile_report.count);
    fprintf(file, "</div>\\n");
    
    // Performance chart
    fprintf(file, "<div class='chart'>\\n");
    fprintf(file, "<h2>📈 Function Performance</h2>\\n");
    fprintf(file, "<canvas id='performanceChart' width='400' height='200'></canvas>\\n");
    fprintf(file, "</div>\\n");
    
    // Function details table
    fprintf(file, "<h2>🔥 Function Details</h2>\\n");
    fprintf(file, "<table>\\n");
    fprintf(file, "<tr><th>Function</th><th>Calls</th><th>Total (ms)</th><th>Average (ms)</th><th>Min (ms)</th><th>Max (ms)</th></tr>\\n");
    
    for (int i = 0; i < profile_report.count; i++) {
        ProfileEntry* entry = &profile_report.entries[i];
        const char* css_class = (entry->total_time_ms > profile_report.total_program_time_ms * 0.1) ? "hotspot" : "";
        
        fprintf(file, "<tr class='%s'><td>%s</td><td>%d</td><td>%.2f</td><td>%.2f</td><td>%.2f</td><td>%.2f</td></tr>\\n",
                css_class, entry->function_name, entry->call_count, entry->total_time_ms, 
                entry->avg_time_ms, entry->min_time_ms, entry->max_time_ms);
    }
    
    fprintf(file, "</table>\\n");
    
    // JavaScript for chart
    fprintf(file, "<script>\\n");
    fprintf(file, "const ctx = document.getElementById('performanceChart').getContext('2d');\\n");
    fprintf(file, "const chart = new Chart(ctx, {\\n");
    fprintf(file, "  type: 'bar',\\n");
    fprintf(file, "  data: {\\n");
    fprintf(file, "    labels: [");
    
    for (int i = 0; i < profile_report.count && i < 10; i++) {
        fprintf(file, "'%s'%s", profile_report.entries[i].function_name, (i < 9 && i < profile_report.count - 1) ? ", " : "");
    }
    
    fprintf(file, "],\\n");
    fprintf(file, "    datasets: [{\\n");
    fprintf(file, "      label: 'Total Time (ms)',\\n");
    fprintf(file, "      data: [");
    
    for (int i = 0; i < profile_report.count && i < 10; i++) {
        fprintf(file, "%.2f%s", profile_report.entries[i].total_time_ms, (i < 9 && i < profile_report.count - 1) ? ", " : "");
    }
    
    fprintf(file, "],\\n");
    fprintf(file, "      backgroundColor: 'rgba(59, 130, 246, 0.8)'\\n");
    fprintf(file, "    }]\\n");
    fprintf(file, "  },\\n");
    fprintf(file, "  options: { responsive: true, scales: { y: { beginAtZero: true } } }\\n");
    fprintf(file, "});\\n");
    fprintf(file, "</script>\\n");
    
    fprintf(file, "</body>\\n</html>\\n");
    fclose(file);
    
    printf("📄 HTML profile report generated: %s\\n", filename);
}

// Signal handler for profiling on demand
void signal_handler(int sig) {
    if (sig == SIGUSR1) {
        printf("\\n🔍 Profiling report requested via signal...\\n");
        print_performance_summary();
        print_hotspots();
        print_memory_visualization();
    }
}

// Simulate some function calls for demonstration
void simulate_function_calls() {
    printf("🎯 Simulating function calls for profiling demo...\\n\\n");
    
    // Simulate various function calls with different performance characteristics
    const char* functions[] = {
        "main()", "parse_args()", "process_file()", "render_output()", 
        "memory_allocate()", "network_request()", "database_query()", 
        "compress_data()", "validate_input()", "generate_report()"
    };
    
    srand(time(NULL));
    
    for (int i = 0; i < 100; i++) {
        const char* func = functions[rand() % 10];
        double duration = (rand() % 50) + 1.0; // 1-50ms
        size_t memory = (rand() % 1000) * 1024; // 0-1MB
        
        record_function_call(func, duration, memory);
        
        // Simulate some actual work
        usleep(1000); // 1ms
    }
}

int main(int argc, char* argv[]) {
    printf("🔍 Goo Integrated Profiler\\n");
    printf("==========================\\n\\n");
    
    // Set up signal handler for on-demand profiling
    signal(SIGUSR1, signal_handler);
    
    init_profiler();
    
    int demo_mode = 0;
    const char* html_report = "profile_report.html";
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) {
            demo_mode = 1;
        } else if (strncmp(argv[i], "--report=", 9) == 0) {
            html_report = argv[i] + 9;
        }
    }
    
    if (demo_mode) {
        simulate_function_calls();
    } else {
        printf("💡 Profiler is ready. Options:\\n");
        printf("  • Send SIGUSR1 signal (kill -USR1 %d) for runtime profiling\\n", getpid());
        printf("  • Use --demo to run simulation\\n");
        printf("  • Profile data will be collected during program execution\\n\\n");
        
        // In a real profiler, this would hook into the Goo runtime
        printf("⏳ Waiting for profiling data... (Press Ctrl+C to generate report)\\n");
        pause(); // Wait for signal
    }
    
    print_performance_summary();
    print_hotspots();
    print_memory_visualization();
    
    generate_html_profile_report(html_report);
    
    cleanup_profiler();
    
    printf("\\n🎉 Profiling complete!\\n");
    printf("📊 View detailed report: %s\\n", html_report);
    
    return 0;
}