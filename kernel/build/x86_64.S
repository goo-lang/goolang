# Goo Kernel Bootstrap for x86_64
# This file provides the minimal assembly code needed to boot the Goo kernel

.section .boot.text, "ax", @progbits
.code32

# Multiboot2 header
.set MULTIBOOT2_MAGIC, 0xE85250D6
.set MULTIBOOT2_ARCH, 0
.set MULTIBOOT2_LENGTH, multiboot2_header_end - multiboot2_header
.set MULTIBOOT2_CHECKSUM, -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_LENGTH)

.align 8
multiboot2_header:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCH
    .long MULTIBOOT2_LENGTH
    .long MULTIBOOT2_CHECKSUM
    
    # End tag
    .short 0
    .short 0
    .long 8
multiboot2_header_end:

# GDT for long mode
.align 16
gdt64:
    .quad 0                     # Null descriptor
    .quad 0x00209A0000000000    # Code segment (64-bit)
    .quad 0x0000920000000000    # Data segment
gdt64_end:

gdt64_descriptor:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

# Page tables for initial mapping
.align 4096
.global boot_pml4
boot_pml4:
    .quad boot_pdpt + 0x003     # Present + Writable
    .fill 510, 8, 0
    .quad boot_pdpt + 0x003     # Recursive mapping

.align 4096
boot_pdpt:
    .quad boot_pd + 0x003       # Present + Writable
    .fill 510, 8, 0
    .quad boot_pd + 0x003       # Higher half mapping

.align 4096
boot_pd:
    # Identity map first 2MB
    .quad 0x000000 + 0x083      # Present + Writable + Large page
    .quad 0x200000 + 0x083      # Present + Writable + Large page
    .fill 510, 8, 0

# Boot stack
.section .boot.data, "aw", @progbits
.align 16
boot_stack_bottom:
    .skip 16384                 # 16KB boot stack
boot_stack_top:

# Entry point
.section .boot.text
.global _start
.type _start, @function
_start:
    # Disable interrupts
    cli
    
    # Set up boot stack
    mov $boot_stack_top, %esp
    
    # Reset EFLAGS
    push $0
    popf
    
    # Check for CPUID support
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax         # Flip ID bit
    push %eax
    popfl
    pushfl
    pop %eax
    cmp %ecx, %eax
    je no_cpuid
    
    # Check for long mode support
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode
    
    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx      # LM bit
    jz no_long_mode
    
    # Enable PAE
    mov %cr4, %eax
    or $0x20, %eax              # Set PAE bit
    mov %eax, %cr4
    
    # Set up page tables
    mov $boot_pml4, %eax
    mov %eax, %cr3
    
    # Enable long mode
    mov $0xC0000080, %ecx       # IA32_EFER MSR
    rdmsr
    or $0x100, %eax             # Set LME bit
    wrmsr
    
    # Enable paging
    mov %cr0, %eax
    or $0x80000000, %eax        # Set PG bit
    mov %eax, %cr0
    
    # Load 64-bit GDT
    lgdt gdt64_descriptor
    
    # Jump to 64-bit code
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    # Set up segment registers
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    
    # Set up higher-half stack
    mov $0xFFFFFFFF80000000, %rax
    add $boot_stack_top, %rax
    mov %rax, %rsp
    
    # Jump to higher half
    mov $0xFFFFFFFF80000000, %rax
    add $higher_half_start, %rax
    jmp *%rax

higher_half_start:
    # Now we're running in the higher half
    # Clear the identity mapping (optional for now)
    
    # Set up proper kernel stack
    extern _stack_top
    mov $_stack_top, %rsp
    
    # Call global constructors
    call _init
    
    # Call kernel main
    extern kernel_main
    call kernel_main
    
    # If kernel_main returns, halt
    cli
1:  hlt
    jmp 1b

# Error handlers
.code32
no_cpuid:
    mov $no_cpuid_msg, %esi
    call print_error
    jmp halt_system

no_long_mode:
    mov $no_long_mode_msg, %esi
    call print_error
    jmp halt_system

print_error:
    mov $0xB8000, %edi          # VGA text buffer
    mov $0x0F00, %ax            # White on black
1:  lodsb
    test %al, %al
    jz 2f
    stosw
    jmp 1b
2:  ret

halt_system:
    cli
1:  hlt
    jmp 1b

# Error messages
no_cpuid_msg:
    .asciz "CPUID not supported"

no_long_mode_msg:
    .asciz "Long mode not supported"

# Global constructors/destructors support
.section .text
.global _init
_init:
    extern _ctors_start
    extern _ctors_end
    mov $_ctors_start, %rax
1:  cmp $_ctors_end, %rax
    jae 2f
    call *(%rax)
    add $8, %rax
    jmp 1b
2:  ret

.global _fini
_fini:
    extern _dtors_start
    extern _dtors_end
    mov $_dtors_end, %rax
1:  cmp $_dtors_start, %rax
    jbe 2f
    sub $8, %rax
    call *(%rax)
    jmp 1b
2:  ret

# Export symbols for linker
.global boot_pml4
.global boot_pdpt
.global boot_pd