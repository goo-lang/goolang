# Goo Kernel Bootstrap for ARM64 (AArch64)
# This file provides the minimal assembly code needed to boot the Goo kernel

.section .boot.text, "ax", %progbits

# Constants for ARM64 boot
.set SCTLR_RESERVED,    (3 << 28) | (3 << 22) | (1 << 20) | (1 << 11)
.set SCTLR_EE_LITTLE_ENDIAN, (0 << 25)
.set SCTLR_EOE_LITTLE_ENDIAN, (0 << 24)
.set SCTLR_I_CACHE_DISABLED, (0 << 12)
.set SCTLR_D_CACHE_DISABLED, (0 << 2)
.set SCTLR_MMU_DISABLED, (0 << 0)
.set SCTLR_VALUE, (SCTLR_RESERVED | SCTLR_EE_LITTLE_ENDIAN | SCTLR_EOE_LITTLE_ENDIAN | SCTLR_I_CACHE_DISABLED | SCTLR_D_CACHE_DISABLED | SCTLR_MMU_DISABLED)

.set HCR_RW, (1 << 31)
.set HCR_VALUE, HCR_RW

.set SCR_RESERVED, (3 << 4)
.set SCR_RW, (1 << 10)
.set SCR_NS, (1 << 0)
.set SCR_VALUE, (SCR_RESERVED | SCR_RW | SCR_NS)

.set SPSR_MASK_ALL, (7 << 6)
.set SPSR_EL1h, (5 << 0)
.set SPSR_VALUE, (SPSR_MASK_ALL | SPSR_EL1h)

# Memory layout constants
.set KERNEL_VMA, 0xFFFF000000000000
.set KERNEL_LMA, 0x0000000040000000

# Entry point
.global _start
.type _start, %function
_start:
    # Disable interrupts
    msr daifset, #0xf
    
    # Check current exception level
    mrs x0, CurrentEL
    and x0, x0, #12
    cmp x0, #12        # EL3?
    beq el3_entry
    cmp x0, #8         # EL2?
    beq el2_entry
    cmp x0, #4         # EL1?
    beq el1_entry
    
    # If we're in EL0, something is wrong
    b hang

el3_entry:
    # Initialize EL3
    msr scr_el3, xzr
    
    # Set up SCR_EL3
    mov x0, #SCR_VALUE
    msr scr_el3, x0
    
    # Set SPSR_EL3 for EL2 entry
    mov x0, #SPSR_VALUE
    msr spsr_el3, x0
    
    # Set return address to EL2 entry
    adr x0, el2_entry
    msr elr_el3, x0
    
    # Return to EL2
    eret

el2_entry:
    # Initialize EL2
    msr hcr_el2, xzr
    
    # Set up HCR_EL2
    mov x0, #HCR_VALUE
    msr hcr_el2, x0
    
    # Set SPSR_EL2 for EL1 entry
    mov x0, #SPSR_VALUE
    msr spsr_el2, x0
    
    # Set return address to EL1 entry
    adr x0, el1_entry
    msr elr_el2, x0
    
    # Initialize generic timer in EL2
    mrs x0, cnthctl_el2
    orr x0, x0, #3          # Enable EL1 access to timers
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr    # Set virtual offset to zero
    
    # Return to EL1
    eret

el1_entry:
    # Now we're in EL1 (kernel mode)
    
    # Set up SCTLR_EL1 (disable MMU, caches)
    mov x0, #SCTLR_VALUE
    msr sctlr_el1, x0
    
    # Clear BSS before we use any global variables
    ldr x0, =_bss_start
    ldr x1, =_bss_end
    sub x1, x1, x0          # Calculate BSS size
    cbz x1, bss_cleared     # Skip if no BSS
clear_bss_loop:
    str xzr, [x0], #8
    sub x1, x1, #8
    cbnz x1, clear_bss_loop
bss_cleared:

    # Set up initial stack
    ldr x0, =_boot_stack_top
    mov sp, x0
    
    # Detect CPU features
    bl detect_cpu_features
    
    # Set up initial page tables
    bl setup_initial_paging
    
    # Enable MMU and caches
    bl enable_mmu
    
    # Jump to higher half
    ldr x0, =higher_half_start
    br x0

higher_half_start:
    # Now running in higher half
    
    # Set up proper kernel stack
    ldr x0, =_stacks_end
    mov sp, x0
    
    # Enable FPU/NEON
    bl enable_fpu
    
    # Call global constructors
    bl _init
    
    # Jump to kernel main
    bl kernel_main
    
    # If kernel_main returns, halt the system
    b hang

# CPU feature detection
detect_cpu_features:
    # Read ID registers to check available features
    mrs x0, ID_AA64PFR0_EL1
    mrs x1, ID_AA64MMFR0_EL1
    mrs x2, ID_AA64DFR0_EL1
    mrs x3, ID_AA64ISAR0_EL1
    
    # Store feature information for later use
    # (This would typically store to global variables)
    
    ret

# Set up initial page tables
setup_initial_paging:
    # Clear page table area
    ldr x0, =_page_tables_start
    ldr x1, =_page_tables_end
    sub x1, x1, x0
clear_page_tables:
    cbz x1, page_tables_cleared
    str xzr, [x0], #8
    sub x1, x1, #8
    b clear_page_tables
page_tables_cleared:

    # Set up level 0 table (PGD)
    ldr x0, =_page_tables_start     # PGD base
    ldr x1, =_page_tables_start
    add x1, x1, #0x1000            # Level 1 table
    orr x1, x1, #0x3               # Present + Table
    str x1, [x0]                   # Identity mapping
    
    # Higher half mapping (last entry)
    add x2, x0, #0xFF8             # Last entry in PGD
    str x1, [x2]
    
    # Set up level 1 table (PUD)
    add x0, x0, #0x1000            # Level 1 table
    ldr x1, =_page_tables_start
    add x1, x1, #0x2000            # Level 2 table
    orr x1, x1, #0x3               # Present + Table
    str x1, [x0]                   # First entry
    
    # Set up level 2 table (PMD) - using 2MB pages
    add x0, x0, #0x1000            # Level 2 table
    mov x1, #0x0                   # Physical address
    orr x1, x1, #0x401             # Present + Block (2MB page)
    mov x2, #0                     # Counter
pmd_loop:
    str x1, [x0, x2, lsl #3]      # Store entry
    add x1, x1, #0x200000         # Next 2MB
    add x2, x2, #1
    cmp x2, #512                  # 512 entries = 1GB
    bne pmd_loop
    
    ret

# Enable MMU and caches
enable_mmu:
    # Set up MAIR (Memory Attribute Indirection Register)
    mov x0, #0xFF                  # Normal memory, Write-back
    msr mair_el1, x0
    
    # Set up TCR (Translation Control Register)
    mov x0, #0x19                  # T0SZ = 25 (39-bit VA)
    orr x0, x0, #(0x19 << 16)     # T1SZ = 25
    orr x0, x0, #(1 << 8)         # IRGN0 = 1 (Inner WB)
    orr x0, x0, #(1 << 10)        # ORGN0 = 1 (Outer WB)
    orr x0, x0, #(3 << 12)        # SH0 = 3 (Inner Shareable)
    orr x0, x0, #(1 << 24)        # IRGN1 = 1
    orr x0, x0, #(1 << 26)        # ORGN1 = 1
    orr x0, x0, #(3 << 28)        # SH1 = 3
    orr x0, x0, #(2 << 32)        # TG1 = 2 (4KB pages)
    msr tcr_el1, x0
    
    # Set TTBR0 and TTBR1
    ldr x0, =_page_tables_start
    msr ttbr0_el1, x0
    msr ttbr1_el1, x0
    
    # Invalidate TLB
    tlbi vmalle1is
    dsb sy
    isb
    
    # Enable MMU, caches, and branch prediction
    mrs x0, sctlr_el1
    orr x0, x0, #(1 << 0)         # MMU enable
    orr x0, x0, #(1 << 2)         # Data cache enable
    orr x0, x0, #(1 << 12)        # Instruction cache enable
    msr sctlr_el1, x0
    
    # Ensure MMU is enabled before continuing
    isb
    
    ret

# Enable FPU/NEON
enable_fpu:
    # Enable access to SIMD/FP registers
    mov x0, #(3 << 20)            # FPEN = 11b (no trapping)
    msr cpacr_el1, x0
    isb
    ret

# Global constructors
.global _init
_init:
    # Call constructors
    ldr x0, =_ctors_start
    ldr x1, =_ctors_end
ctor_loop:
    cmp x0, x1
    beq ctor_done
    ldr x2, [x0], #8
    cbz x2, ctor_loop          # Skip null entries
    blr x2
    b ctor_loop
ctor_done:
    ret

# Global destructors
.global _fini
_fini:
    # Call destructors in reverse order
    ldr x0, =_dtors_end
    ldr x1, =_dtors_start
dtor_loop:
    cmp x0, x1
    beq dtor_done
    sub x0, x0, #8
    ldr x2, [x0]
    cbz x2, dtor_loop          # Skip null entries
    blr x2
    b dtor_loop
dtor_done:
    ret

# Exception vector table
.align 11
.global exception_vectors
exception_vectors:
    # Current EL with SP0
    .align 7
    b exception_handler         # Synchronous
    .align 7
    b exception_handler         # IRQ
    .align 7
    b exception_handler         # FIQ
    .align 7
    b exception_handler         # SError
    
    # Current EL with SPx
    .align 7
    b exception_handler         # Synchronous
    .align 7
    b irq_handler              # IRQ
    .align 7
    b exception_handler         # FIQ
    .align 7
    b exception_handler         # SError
    
    # Lower EL using AArch64
    .align 7
    b syscall_handler          # Synchronous (syscalls)
    .align 7
    b exception_handler         # IRQ
    .align 7
    b exception_handler         # FIQ
    .align 7
    b exception_handler         # SError
    
    # Lower EL using AArch32
    .align 7
    b exception_handler         # Synchronous
    .align 7
    b exception_handler         # IRQ
    .align 7
    b exception_handler         # FIQ
    .align 7
    b exception_handler         # SError

# Exception handlers
exception_handler:
    # Save registers
    stp x0, x1, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    stp x8, x9, [sp, #-16]!
    stp x10, x11, [sp, #-16]!
    stp x12, x13, [sp, #-16]!
    stp x14, x15, [sp, #-16]!
    stp x16, x17, [sp, #-16]!
    stp x18, x19, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    stp x22, x23, [sp, #-16]!
    stp x24, x25, [sp, #-16]!
    stp x26, x27, [sp, #-16]!
    stp x28, x29, [sp, #-16]!
    str x30, [sp, #-8]!
    
    # Call C exception handler
    mov x0, sp                  # Pass register context
    bl handle_exception
    
    # Restore registers
    ldr x30, [sp], #8
    ldp x28, x29, [sp], #16
    ldp x26, x27, [sp], #16
    ldp x24, x25, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x18, x19, [sp], #16
    ldp x16, x17, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x8, x9, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x0, x1, [sp], #16
    
    eret

irq_handler:
    # Similar to exception_handler but calls handle_irq
    stp x0, x1, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    stp x8, x9, [sp, #-16]!
    stp x10, x11, [sp, #-16]!
    stp x12, x13, [sp, #-16]!
    stp x14, x15, [sp, #-16]!
    stp x16, x17, [sp, #-16]!
    stp x18, x19, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    stp x22, x23, [sp, #-16]!
    stp x24, x25, [sp, #-16]!
    stp x26, x27, [sp, #-16]!
    stp x28, x29, [sp, #-16]!
    str x30, [sp, #-8]!
    
    # Call C IRQ handler
    mov x0, sp
    bl handle_irq
    
    # Restore registers
    ldr x30, [sp], #8
    ldp x28, x29, [sp], #16
    ldp x26, x27, [sp], #16
    ldp x24, x25, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x18, x19, [sp], #16
    ldp x16, x17, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x8, x9, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x0, x1, [sp], #16
    
    eret

syscall_handler:
    # Handle system calls from user space
    stp x0, x1, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    stp x8, x9, [sp, #-16]!
    stp x10, x11, [sp, #-16]!
    stp x12, x13, [sp, #-16]!
    stp x14, x15, [sp, #-16]!
    stp x16, x17, [sp, #-16]!
    stp x18, x19, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    stp x22, x23, [sp, #-16]!
    stp x24, x25, [sp, #-16]!
    stp x26, x27, [sp, #-16]!
    stp x28, x29, [sp, #-16]!
    str x30, [sp, #-8]!
    
    # Call C syscall handler
    mov x0, sp
    bl handle_syscall
    
    # Restore registers
    ldr x30, [sp], #8
    ldp x28, x29, [sp], #16
    ldp x26, x27, [sp], #16
    ldp x24, x25, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x18, x19, [sp], #16
    ldp x16, x17, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x8, x9, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x0, x1, [sp], #16
    
    eret

# Hang the system
hang:
    wfi                         # Wait for interrupt
    b hang                      # Loop forever

# Boot data section
.section .boot.data, "aw", %progbits
.align 16
_boot_stack_bottom:
    .skip 65536                 # 64KB boot stack
_boot_stack_top:

# CPU feature storage
.align 8
cpu_features:
    .quad 0                     # ID_AA64PFR0_EL1
    .quad 0                     # ID_AA64MMFR0_EL1
    .quad 0                     # ID_AA64DFR0_EL1
    .quad 0                     # ID_AA64ISAR0_EL1

# Export symbols for linker
.global _boot_stack_bottom
.global _boot_stack_top
.global exception_vectors
.global cpu_features

# External functions that need to be provided by kernel
.extern kernel_main
.extern handle_exception
.extern handle_irq
.extern handle_syscall

# Linker-provided symbols
.extern _bss_start
.extern _bss_end
.extern _page_tables_start
.extern _page_tables_end
.extern _stacks_end
.extern _ctors_start
.extern _ctors_end
.extern _dtors_start
.extern _dtors_end