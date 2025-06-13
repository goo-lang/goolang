#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <signal.h>
#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <libdwarf/dwarf.h>
#include <libdwarf/libdwarf.h>

// Goo Debugger Implementation
// Provides debugging support for Goo programs with DWARF integration

#define MAX_PATH_LENGTH 1024
#define MAX_SYMBOL_NAME 256
#define MAX_BREAKPOINTS 100
#define MAX_STACK_FRAMES 50
#define MAX_VARIABLES 200

// Breakpoint structure
typedef struct {
    unsigned long address;
    unsigned long original_data;
    int enabled;
    char file[MAX_PATH_LENGTH];
    int line;
    char condition[256];
    int hit_count;
} Breakpoint;

// Stack frame information
typedef struct {
    unsigned long frame_pointer;
    unsigned long instruction_pointer;
    unsigned long return_address;
    char function_name[MAX_SYMBOL_NAME];
    char file_name[MAX_PATH_LENGTH];
    int line_number;
    int frame_level;
} StackFrame;

// Variable information
typedef struct {
    char name[MAX_SYMBOL_NAME];
    char type[MAX_SYMBOL_NAME];
    unsigned long address;
    size_t size;
    char value[512];
    int scope_level;
    int is_parameter;
} Variable;

// Debug session state
typedef struct {
    pid_t target_pid;
    int target_status;
    char executable_path[MAX_PATH_LENGTH];
    
    // DWARF debugging information
    Dwarf_Debug dwarf_info;
    Dwarf_Error dwarf_error;
    int dwarf_fd;
    
    // Breakpoints
    Breakpoint breakpoints[MAX_BREAKPOINTS];
    int breakpoint_count;
    int current_breakpoint;
    
    // Stack and variables
    StackFrame stack_frames[MAX_STACK_FRAMES];
    int stack_frame_count;
    int current_frame;
    
    Variable variables[MAX_VARIABLES];
    int variable_count;
    
    // Execution state
    int is_running;
    int is_attached;
    unsigned long current_pc;
    struct user_regs_struct registers;
    
    // Configuration
    int step_mode;  // 0=into, 1=over, 2=out
    int auto_list_source;
    int print_registers;
    int verbose;
} DebugSession;

static DebugSession session = {0};

// Function prototypes
void debugger_init(void);
void debugger_cleanup(void);
void show_help(void);
void interactive_mode(void);
int load_executable(const char* path);
int start_target(const char* executable, char* const argv[]);
int attach_to_process(pid_t pid);
int detach_from_process(void);

// Breakpoint management
int set_breakpoint(const char* location);
int set_breakpoint_at_address(unsigned long address);
int set_breakpoint_at_line(const char* file, int line);
int remove_breakpoint(int bp_id);
int enable_breakpoint(int bp_id);
int disable_breakpoint(int bp_id);
void list_breakpoints(void);
int check_breakpoint_hit(void);

// Execution control
int continue_execution(void);
int step_instruction(void);
int step_into(void);
int step_over(void);
int step_out(void);
int interrupt_execution(void);

// Stack and variable inspection
int update_stack_trace(void);
int print_stack_trace(void);
int select_frame(int frame_num);
int update_variables(void);
int print_variables(void);
int print_variable(const char* name);
int set_variable(const char* name, const char* value);

// Memory inspection
int examine_memory(unsigned long address, int count, char format);
int disassemble(unsigned long address, int count);

// DWARF information
int load_dwarf_info(const char* executable);
int find_line_address(const char* file, int line, unsigned long* address);
int find_function_info(unsigned long address, char* function_name, char* file_name, int* line_number);
int find_variable_info(const char* name, unsigned long frame_base, Variable* var);
int get_source_line(const char* file, int line, char* buffer, size_t buffer_size);

// Utilities
void print_registers(void);
void print_error(const char* message);
void print_info(const char* format, ...);
unsigned long read_target_memory(unsigned long address);
int write_target_memory(unsigned long address, unsigned long data);
char* trim_whitespace(char* str);

int main(int argc, char* argv[]) {
    const char* executable = NULL;
    const char* core_file = NULL;
    pid_t attach_pid = 0;
    char** target_argv = NULL;
    int target_argc = 0;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            show_help();
            return 0;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pid") == 0) {
            if (i + 1 < argc) {
                attach_pid = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--core") == 0) {
            if (i + 1 < argc) {
                core_file = argv[++i];
            }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            session.verbose = 1;
        } else if (strcmp(argv[i], "--") == 0) {
            // Rest of arguments are for target program
            target_argc = argc - i - 1;
            target_argv = &argv[i + 1];
            break;
        } else if (!executable) {
            executable = argv[i];
        }
    }
    
    if (!executable && !attach_pid && !core_file) {
        printf("Goo Debugger v0.1.0\n\n");
        printf("Usage: goo-debug [options] <executable> [-- <args>]\n\n");
        show_help();
        return 1;
    }
    
    // Initialize debugger
    debugger_init();
    
    print_info("Goo Debugger starting...");
    
    if (attach_pid) {
        // Attach to existing process
        if (attach_to_process(attach_pid) != 0) {
            print_error("Failed to attach to process");
            return 1;
        }
        print_info("Attached to process %d", attach_pid);
    } else if (core_file) {
        // Analyze core dump
        print_error("Core dump analysis not implemented");
        return 1;
    } else {
        // Load and start executable
        if (load_executable(executable) != 0) {
            print_error("Failed to load executable");
            return 1;
        }
        
        if (start_target(executable, target_argv) != 0) {
            print_error("Failed to start target process");
            return 1;
        }
        
        print_info("Started process %d", session.target_pid);
    }
    
    // Enter interactive mode
    interactive_mode();
    
    // Cleanup
    debugger_cleanup();
    print_info("Debugger session ended");
    
    return 0;
}

void debugger_init(void) {
    memset(&session, 0, sizeof(session));
    session.auto_list_source = 1;
    session.current_frame = 0;
}

void debugger_cleanup(void) {
    if (session.is_attached) {
        detach_from_process();
    }
    
    if (session.dwarf_info) {
        dwarf_finish(session.dwarf_info, &session.dwarf_error);
    }
    
    if (session.dwarf_fd >= 0) {
        close(session.dwarf_fd);
    }
}

void show_help(void) {
    printf("Debug Goo programs with DWARF debugging information.\n\n");
    printf("Options:\n");
    printf("  -p, --pid <pid>          Attach to running process\n");
    printf("  -c, --core <file>        Analyze core dump\n");
    printf("  -v, --verbose            Enable verbose output\n");
    printf("  -h, --help               Show this help message\n");
    
    printf("\nCommands (interactive mode):\n");
    printf("  run [args]               Start program execution\n");
    printf("  continue, c              Continue execution\n");
    printf("  step, s                  Step one instruction\n");
    printf("  stepi, si                Step into function calls\n");
    printf("  next, n                  Step over function calls\n");
    printf("  finish                   Step out of current function\n");
    printf("  break <location>         Set breakpoint\n");
    printf("  delete <bp-id>           Delete breakpoint\n");
    printf("  disable <bp-id>          Disable breakpoint\n");
    printf("  enable <bp-id>           Enable breakpoint\n");
    printf("  info breakpoints         List breakpoints\n");
    printf("  backtrace, bt            Show stack trace\n");
    printf("  frame <num>              Select stack frame\n");
    printf("  info locals              Show local variables\n");
    printf("  print <var>              Print variable value\n");
    printf("  set <var>=<value>        Set variable value\n");
    printf("  examine <addr>           Examine memory\n");
    printf("  disassemble <addr>       Disassemble instructions\n");
    printf("  info registers           Show CPU registers\n");
    printf("  list [line]              List source code\n");
    printf("  quit, q                  Exit debugger\n");
    
    printf("\nExamples:\n");
    printf("  goo-debug my_program                    # Debug program\n");
    printf("  goo-debug my_program -- arg1 arg2      # Debug with arguments\n");
    printf("  goo-debug -p 1234                      # Attach to process 1234\n");
    printf("  goo-debug -c core.dump my_program      # Analyze core dump\n");
}

void interactive_mode(void) {
    char command_line[1024];
    char* command;
    char* args;
    
    printf("(goo-debug) ");
    
    while (fgets(command_line, sizeof(command_line), stdin)) {
        // Trim whitespace
        command = trim_whitespace(command_line);
        
        if (strlen(command) == 0) {
            printf("(goo-debug) ");
            continue;
        }
        
        // Split command and arguments
        args = strchr(command, ' ');
        if (args) {
            *args = '\0';
            args = trim_whitespace(args + 1);
        }
        
        // Process commands
        if (strcmp(command, "quit") == 0 || strcmp(command, "q") == 0) {
            break;
        } else if (strcmp(command, "help") == 0 || strcmp(command, "h") == 0) {
            show_help();
        } else if (strcmp(command, "run") == 0 || strcmp(command, "r") == 0) {
            if (!session.is_running) {
                continue_execution();
            } else {
                print_info("Program already running");
            }
        } else if (strcmp(command, "continue") == 0 || strcmp(command, "c") == 0) {
            continue_execution();
        } else if (strcmp(command, "step") == 0 || strcmp(command, "s") == 0) {
            step_instruction();
        } else if (strcmp(command, "stepi") == 0 || strcmp(command, "si") == 0) {
            step_into();
        } else if (strcmp(command, "next") == 0 || strcmp(command, "n") == 0) {
            step_over();
        } else if (strcmp(command, "finish") == 0) {
            step_out();
        } else if (strcmp(command, "break") == 0 || strcmp(command, "b") == 0) {
            if (args) {
                set_breakpoint(args);
            } else {
                print_error("Usage: break <location>");
            }
        } else if (strcmp(command, "delete") == 0) {
            if (args) {
                int bp_id = atoi(args);
                remove_breakpoint(bp_id);
            } else {
                print_error("Usage: delete <breakpoint-id>");
            }
        } else if (strcmp(command, "disable") == 0) {
            if (args) {
                int bp_id = atoi(args);
                disable_breakpoint(bp_id);
            } else {
                print_error("Usage: disable <breakpoint-id>");
            }
        } else if (strcmp(command, "enable") == 0) {
            if (args) {
                int bp_id = atoi(args);
                enable_breakpoint(bp_id);
            } else {
                print_error("Usage: enable <breakpoint-id>");
            }
        } else if (strcmp(command, "info") == 0) {
            if (args) {
                if (strcmp(args, "breakpoints") == 0) {
                    list_breakpoints();
                } else if (strcmp(args, "locals") == 0) {
                    update_variables();
                    print_variables();
                } else if (strcmp(args, "registers") == 0) {
                    print_registers();
                } else {
                    print_error("Unknown info command: %s", args);
                }
            } else {
                print_error("Usage: info <breakpoints|locals|registers>");
            }
        } else if (strcmp(command, "backtrace") == 0 || strcmp(command, "bt") == 0) {
            update_stack_trace();
            print_stack_trace();
        } else if (strcmp(command, "frame") == 0) {
            if (args) {
                int frame_num = atoi(args);
                select_frame(frame_num);
            } else {
                print_error("Usage: frame <frame-number>");
            }
        } else if (strcmp(command, "print") == 0 || strcmp(command, "p") == 0) {
            if (args) {
                print_variable(args);
            } else {
                print_error("Usage: print <variable-name>");
            }
        } else if (strcmp(command, "set") == 0) {
            if (args) {
                char* equals = strchr(args, '=');
                if (equals) {
                    *equals = '\0';
                    char* var_name = trim_whitespace(args);
                    char* value = trim_whitespace(equals + 1);
                    set_variable(var_name, value);
                } else {
                    print_error("Usage: set <variable>=<value>");
                }
            } else {
                print_error("Usage: set <variable>=<value>");
            }
        } else if (strcmp(command, "examine") == 0 || strcmp(command, "x") == 0) {
            if (args) {
                unsigned long address = strtoul(args, NULL, 0);
                examine_memory(address, 8, 'x');
            } else {
                examine_memory(session.current_pc, 8, 'x');
            }
        } else if (strcmp(command, "disassemble") == 0 || strcmp(command, "disas") == 0) {
            if (args) {
                unsigned long address = strtoul(args, NULL, 0);
                disassemble(address, 10);
            } else {
                disassemble(session.current_pc, 10);
            }
        } else if (strcmp(command, "list") == 0 || strcmp(command, "l") == 0) {
            // List source code around current location
            char file_name[MAX_PATH_LENGTH];
            int line_number;
            
            if (find_function_info(session.current_pc, NULL, file_name, &line_number) == 0) {
                char line_buffer[512];
                for (int i = line_number - 5; i <= line_number + 5; i++) {
                    if (i > 0 && get_source_line(file_name, i, line_buffer, sizeof(line_buffer)) == 0) {
                        char marker = (i == line_number) ? '>' : ' ';
                        printf("%c %4d  %s", marker, i, line_buffer);
                    }
                }
            } else {
                print_error("No source information available");
            }
        } else if (strcmp(command, "interrupt") == 0) {
            interrupt_execution();
        } else {
            print_error("Unknown command: %s (type 'help' for available commands)", command);
        }
        
        printf("(goo-debug) ");
    }
}

int load_executable(const char* path) {
    strncpy(session.executable_path, path, sizeof(session.executable_path) - 1);
    
    // Load DWARF debugging information
    if (load_dwarf_info(path) != 0) {
        print_error("Warning: Could not load debugging information");
        // Continue without DWARF info
    }
    
    return 0;
}

int start_target(const char* executable, char* const argv[]) {
    pid_t pid = fork();
    
    if (pid == 0) {
        // Child process - prepare for debugging
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
            perror("ptrace");
            exit(1);
        }
        
        // Execute target program
        if (argv) {
            execv(executable, argv);
        } else {
            char* default_argv[] = {(char*)executable, NULL};
            execv(executable, default_argv);
        }
        
        perror("execv");
        exit(1);
    } else if (pid > 0) {
        // Parent process - debugger
        session.target_pid = pid;
        session.is_attached = 1;
        
        // Wait for child to stop
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFSTOPPED(status)) {
            session.target_status = status;
            
            // Get initial state
            if (ptrace(PTRACE_GETREGS, pid, NULL, &session.registers) == 0) {
                session.current_pc = session.registers.rip;
            }
            
            print_info("Process stopped at entry point: 0x%lx", session.current_pc);
            return 0;
        } else {
            print_error("Target process exited unexpectedly");
            return -1;
        }
    } else {
        perror("fork");
        return -1;
    }
}

int attach_to_process(pid_t pid) {
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) {
        perror("ptrace attach");
        return -1;
    }
    
    session.target_pid = pid;
    session.is_attached = 1;
    
    // Wait for process to stop
    int status;
    waitpid(pid, &status, 0);
    session.target_status = status;
    
    // Get process state
    if (ptrace(PTRACE_GETREGS, pid, NULL, &session.registers) == 0) {
        session.current_pc = session.registers.rip;
    }
    
    return 0;
}

int detach_from_process(void) {
    if (session.is_attached) {
        ptrace(PTRACE_DETACH, session.target_pid, NULL, NULL);
        session.is_attached = 0;
    }
    return 0;
}

int set_breakpoint(const char* location) {
    // Parse breakpoint location
    if (strchr(location, ':')) {
        // File:line format
        char file[MAX_PATH_LENGTH];
        int line;
        
        if (sscanf(location, "%[^:]:%d", file, &line) == 2) {
            return set_breakpoint_at_line(file, line);
        }
    } else if (strncmp(location, "0x", 2) == 0) {
        // Hexadecimal address
        unsigned long address = strtoul(location, NULL, 16);
        return set_breakpoint_at_address(address);
    } else if (isdigit(location[0])) {
        // Line number in current file
        int line = atoi(location);
        return set_breakpoint_at_line("", line); // Current file
    } else {
        // Function name
        print_error("Function name breakpoints not implemented");
        return -1;
    }
    
    return -1;
}

int set_breakpoint_at_address(unsigned long address) {
    if (session.breakpoint_count >= MAX_BREAKPOINTS) {
        print_error("Maximum number of breakpoints reached");
        return -1;
    }
    
    // Check if breakpoint already exists
    for (int i = 0; i < session.breakpoint_count; i++) {
        if (session.breakpoints[i].address == address) {
            print_error("Breakpoint already exists at 0x%lx", address);
            return -1;
        }
    }
    
    // Read original instruction
    errno = 0;
    unsigned long original_data = ptrace(PTRACE_PEEKTEXT, session.target_pid, address, NULL);
    if (errno != 0) {
        perror("ptrace peek");
        return -1;
    }
    
    // Set breakpoint instruction (int3 = 0xCC)
    unsigned long breakpoint_data = (original_data & ~0xFF) | 0xCC;
    if (ptrace(PTRACE_POKETEXT, session.target_pid, address, breakpoint_data) == -1) {
        perror("ptrace poke");
        return -1;
    }
    
    // Add to breakpoint list
    Breakpoint* bp = &session.breakpoints[session.breakpoint_count];
    bp->address = address;
    bp->original_data = original_data;
    bp->enabled = 1;
    bp->hit_count = 0;
    
    int bp_id = session.breakpoint_count;
    session.breakpoint_count++;
    
    print_info("Breakpoint %d set at 0x%lx", bp_id, address);
    return bp_id;
}

int set_breakpoint_at_line(const char* file, int line) {
    unsigned long address;
    
    if (find_line_address(file, line, &address) == 0) {
        return set_breakpoint_at_address(address);
    } else {
        print_error("Could not find address for %s:%d", file, line);
        return -1;
    }
}

int continue_execution(void) {
    if (!session.is_attached) {
        print_error("No process attached");
        return -1;
    }
    
    // Check if we're at a breakpoint
    int bp_hit = check_breakpoint_hit();
    if (bp_hit >= 0) {
        // Step past breakpoint
        Breakpoint* bp = &session.breakpoints[bp_hit];
        
        // Restore original instruction
        ptrace(PTRACE_POKETEXT, session.target_pid, bp->address, bp->original_data);
        
        // Step one instruction
        ptrace(PTRACE_SINGLESTEP, session.target_pid, NULL, NULL);
        
        int status;
        waitpid(session.target_pid, &status, 0);
        
        // Restore breakpoint
        unsigned long breakpoint_data = (bp->original_data & ~0xFF) | 0xCC;
        ptrace(PTRACE_POKETEXT, session.target_pid, bp->address, breakpoint_data);
    }
    
    // Continue execution
    session.is_running = 1;
    ptrace(PTRACE_CONT, session.target_pid, NULL, NULL);
    
    int status;
    waitpid(session.target_pid, &status, 0);
    session.target_status = status;
    session.is_running = 0;
    
    if (WIFSTOPPED(status)) {
        // Update registers
        ptrace(PTRACE_GETREGS, session.target_pid, NULL, &session.registers);
        session.current_pc = session.registers.rip;
        
        // Check for breakpoint hit
        bp_hit = check_breakpoint_hit();
        if (bp_hit >= 0) {
            print_info("Breakpoint %d hit at 0x%lx", bp_hit, session.current_pc - 1);
            session.breakpoints[bp_hit].hit_count++;
            
            // Adjust PC to point to actual instruction
            session.current_pc--;
            session.registers.rip--;
            ptrace(PTRACE_SETREGS, session.target_pid, NULL, &session.registers);
            
            // Show source if available
            if (session.auto_list_source) {
                char file_name[MAX_PATH_LENGTH];
                int line_number;
                
                if (find_function_info(session.current_pc, NULL, file_name, &line_number) == 0) {
                    print_info("Stopped at %s:%d", file_name, line_number);
                    
                    char line_buffer[512];
                    if (get_source_line(file_name, line_number, line_buffer, sizeof(line_buffer)) == 0) {
                        printf("=> %d  %s", line_number, line_buffer);
                    }
                }
            }
        } else {
            print_info("Process stopped at 0x%lx (signal %d)", session.current_pc, WSTOPSIG(status));
        }
    } else if (WIFEXITED(status)) {
        print_info("Process exited with code %d", WEXITSTATUS(status));
        session.is_attached = 0;
    } else {
        print_info("Process terminated by signal %d", WTERMSIG(status));
        session.is_attached = 0;
    }
    
    return 0;
}

int step_instruction(void) {
    if (!session.is_attached) {
        print_error("No process attached");
        return -1;
    }
    
    ptrace(PTRACE_SINGLESTEP, session.target_pid, NULL, NULL);
    
    int status;
    waitpid(session.target_pid, &status, 0);
    session.target_status = status;
    
    if (WIFSTOPPED(status)) {
        ptrace(PTRACE_GETREGS, session.target_pid, NULL, &session.registers);
        session.current_pc = session.registers.rip;
        print_info("Stepped to 0x%lx", session.current_pc);
    }
    
    return 0;
}

void list_breakpoints(void) {
    if (session.breakpoint_count == 0) {
        print_info("No breakpoints set");
        return;
    }
    
    printf("Breakpoints:\n");
    for (int i = 0; i < session.breakpoint_count; i++) {
        Breakpoint* bp = &session.breakpoints[i];
        printf("  %d: 0x%lx %s (hits: %d)\n", 
               i, bp->address, 
               bp->enabled ? "enabled" : "disabled",
               bp->hit_count);
    }
}

int check_breakpoint_hit(void) {
    for (int i = 0; i < session.breakpoint_count; i++) {
        if (session.breakpoints[i].enabled && 
            session.current_pc - 1 == session.breakpoints[i].address) {
            return i;
        }
    }
    return -1;
}

void print_registers(void) {
    printf("Registers:\n");
    printf("  rax = 0x%016llx    rbx = 0x%016llx    rcx = 0x%016llx\n", 
           session.registers.rax, session.registers.rbx, session.registers.rcx);
    printf("  rdx = 0x%016llx    rsi = 0x%016llx    rdi = 0x%016llx\n",
           session.registers.rdx, session.registers.rsi, session.registers.rdi);
    printf("  rbp = 0x%016llx    rsp = 0x%016llx    rip = 0x%016llx\n",
           session.registers.rbp, session.registers.rsp, session.registers.rip);
    printf("  eflags = 0x%08llx\n", session.registers.eflags);
}

int load_dwarf_info(const char* executable) {
    session.dwarf_fd = open(executable, O_RDONLY);
    if (session.dwarf_fd < 0) {
        return -1;
    }
    
    int result = dwarf_init(session.dwarf_fd, DW_DLC_READ, NULL, NULL, 
                           &session.dwarf_info, &session.dwarf_error);
    
    if (result != DW_DLV_OK) {
        close(session.dwarf_fd);
        session.dwarf_fd = -1;
        return -1;
    }
    
    print_info("Loaded DWARF debugging information");
    return 0;
}

int find_line_address(const char* file, int line, unsigned long* address) {
    // Simplified line-to-address mapping
    // In a real implementation, this would use DWARF line number information
    *address = 0x400000 + line * 16; // Dummy calculation
    return 0;
}

int find_function_info(unsigned long address, char* function_name, char* file_name, int* line_number) {
    // Simplified address-to-source mapping
    // In a real implementation, this would use DWARF information
    if (function_name) strcpy(function_name, "main");
    if (file_name) strcpy(file_name, "main.goo");
    if (line_number) *line_number = (address - 0x400000) / 16;
    return 0;
}

int get_source_line(const char* file, int line, char* buffer, size_t buffer_size) {
    FILE* f = fopen(file, "r");
    if (!f) return -1;
    
    char line_buffer[1024];
    int current_line = 1;
    
    while (fgets(line_buffer, sizeof(line_buffer), f)) {
        if (current_line == line) {
            strncpy(buffer, line_buffer, buffer_size - 1);
            buffer[buffer_size - 1] = '\0';
            fclose(f);
            return 0;
        }
        current_line++;
    }
    
    fclose(f);
    return -1;
}

void print_error(const char* message) {
    fprintf(stderr, "Error: %s\n", message);
}

void print_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf("Info: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

char* trim_whitespace(char* str) {
    char* end;
    
    // Trim leading space
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) return str;
    
    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    end[1] = '\0';
    return str;
}

// Stub implementations for remaining functions
int step_into(void) { return step_instruction(); }
int step_over(void) { return step_instruction(); }
int step_out(void) { return continue_execution(); }
int interrupt_execution(void) { return kill(session.target_pid, SIGINT); }
int remove_breakpoint(int bp_id) { return 0; }
int enable_breakpoint(int bp_id) { return 0; }
int disable_breakpoint(int bp_id) { return 0; }
int update_stack_trace(void) { return 0; }
int print_stack_trace(void) { printf("Stack trace not implemented\n"); return 0; }
int select_frame(int frame_num) { return 0; }
int update_variables(void) { return 0; }
int print_variables(void) { printf("Variable inspection not implemented\n"); return 0; }
int print_variable(const char* name) { return 0; }
int set_variable(const char* name, const char* value) { return 0; }
int examine_memory(unsigned long address, int count, char format) { return 0; }
int disassemble(unsigned long address, int count) { return 0; }