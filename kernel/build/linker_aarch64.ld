/* Goo Kernel Linker Script for AArch64 */

ENTRY(_start)

/* Define the kernel's virtual and physical addresses */
KERNEL_VMA = 0xFFFF000000000000;  /* Virtual address (upper half) */
KERNEL_LMA = 0x0000000040000000;  /* Physical address (1GB, typical for ARM) */

/* Memory layout */
MEMORY
{
    /* Kernel memory */
    kernel (rwx) : ORIGIN = 0x40000000, LENGTH = 512M
    
    /* Device memory region */
    device (rw) : ORIGIN = 0x80000000, LENGTH = 512M
}

SECTIONS
{
    /* Boot section (identity mapped initially) */
    .boot : AT(0x40000000)
    {
        KEEP(*(.boot.text))
        KEEP(*(.boot.data))
        . = ALIGN(4K);
    } >kernel
    
    /* Main kernel sections start at higher half */
    . = KERNEL_VMA;
    
    /* Text section */
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA)
    {
        _text_start = .;
        KEEP(*(.text.start))  /* Entry point */
        *(.text)
        *(.text.*)
        *(.gnu.linkonce.t.*)
        
        /* ARM64 specific sections */
        *(.text.hot)
        *(.text.unlikely)
        *(.text.startup)
        *(.text.exit)
        
        _text_end = .;
    } >kernel
    
    /* Exception vectors (ARM64 specific) */
    .vectors ALIGN(2K) : AT(ADDR(.vectors) - KERNEL_VMA + KERNEL_LMA)
    {
        _vectors_start = .;
        KEEP(*(.vectors))
        . = ALIGN(2K);
        _vectors_end = .;
    } >kernel
    
    /* Read-only data */
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA)
    {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        *(.gnu.linkonce.r.*)
        
        /* String literals */
        *(.rodata.str1.1)
        *(.rodata.str1.8)
        *(.rodata.str1.16)
        
        /* Device tree blob (if embedded) */
        . = ALIGN(8);
        _dtb_start = .;
        *(.dtb)
        _dtb_end = .;
        
        /* Constructor and destructor tables */
        . = ALIGN(8);
        _ctors_start = .;
        KEEP(*(.ctors))
        KEEP(*(.init_array))
        _ctors_end = .;
        
        _dtors_start = .;
        KEEP(*(.dtors))
        KEEP(*(.fini_array))
        _dtors_end = .;
        
        _rodata_end = .;
    } >kernel
    
    /* ARM64 unwind tables */
    .ARM.exidx ALIGN(8) : AT(ADDR(.ARM.exidx) - KERNEL_VMA + KERNEL_LMA)
    {
        _exidx_start = .;
        *(.ARM.exidx*)
        _exidx_end = .;
    } >kernel
    
    .ARM.extab ALIGN(8) : AT(ADDR(.ARM.extab) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.ARM.extab*)
    } >kernel
    
    /* Initialized data */
    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA + KERNEL_LMA)
    {
        _data_start = .;
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d.*)
        
        /* Global offset table */
        . = ALIGN(8);
        _got_start = .;
        *(.got)
        *(.got.plt)
        _got_end = .;
        
        /* Small data sections (ARM64) */
        *(.sdata)
        *(.sdata.*)
        
        _data_end = .;
    } >kernel
    
    /* Uninitialized data */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA)
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        
        /* Small BSS (ARM64) */
        *(.sbss)
        *(.sbss.*)
        
        . = ALIGN(4K);
        _bss_end = .;
    } >kernel
    
    /* Initial page tables (ARM64 requires specific alignment) */
    .page_tables ALIGN(4K) : AT(ADDR(.page_tables) - KERNEL_VMA + KERNEL_LMA)
    {
        _page_tables_start = .;
        . += 64K;  /* Space for initial page tables */
        _page_tables_end = .;
    } >kernel
    
    /* Kernel heap (reserve space) */
    .heap ALIGN(4K) : AT(ADDR(.heap) - KERNEL_VMA + KERNEL_LMA)
    {
        _heap_start = .;
        . += 32M;  /* 32MB heap */
        _heap_end = .;
    } >kernel
    
    /* Kernel stacks (one per CPU) */
    .stacks ALIGN(4K) : AT(ADDR(.stacks) - KERNEL_VMA + KERNEL_LMA)
    {
        _stacks_start = .;
        
        /* Boot CPU stack */
        _boot_stack_bottom = .;
        . += 64K;  /* 64KB boot stack */
        _boot_stack_top = .;
        
        /* Space for additional CPU stacks */
        . += 64K * 15;  /* 15 more CPUs, 64KB each */
        
        _stacks_end = .;
    } >kernel
    
    /* CPU-local data */
    .percpu ALIGN(4K) : AT(ADDR(.percpu) - KERNEL_VMA + KERNEL_LMA)
    {
        _percpu_start = .;
        *(.data..percpu)
        . = ALIGN(4K);
        _percpu_end = .;
    } >kernel
    
    /* Kernel end marker */
    _kernel_start = KERNEL_VMA;
    _kernel_end = .;
    _kernel_size = _kernel_end - _kernel_start;
    
    /* Physical addresses */
    _kernel_phys_start = KERNEL_LMA;
    _kernel_phys_end = _kernel_phys_start + _kernel_size;
    
    /* Memory map information for kernel */
    _memory_map_start = _kernel_phys_end;
    _memory_map_end = _memory_map_start + 4K;  /* Reserve 4K for memory map */
    
    /* Device tree location (if not embedded) */
    _dtb_phys_start = _memory_map_end;
    _dtb_phys_end = _dtb_phys_start + 64K;  /* Reserve 64K for device tree */
    
    /* Free memory starts after reserved areas */
    _free_memory_start = _dtb_phys_end;
    
    /* Debug sections (not loaded) */
    .debug_info     0 : { *(.debug_info) }
    .debug_abbrev   0 : { *(.debug_abbrev) }
    .debug_line     0 : { *(.debug_line) }
    .debug_str      0 : { *(.debug_str) }
    .debug_ranges   0 : { *(.debug_ranges) }
    .debug_loc      0 : { *(.debug_loc) }
    .debug_frame    0 : { *(.debug_frame) }
    .debug_macinfo  0 : { *(.debug_macinfo) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_pubtypes 0 : { *(.debug_pubtypes) }
    .debug_aranges  0 : { *(.debug_aranges) }
    
    /* ARM64 specific debug sections */
    .debug_gdb_scripts 0 : { *(.debug_gdb_scripts) }
    
    /* Symbol tables (not loaded) */
    .symtab 0 : { *(.symtab) }
    .strtab 0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }
    
    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.ARM.attributes)
        *(.gnu.attributes)
    }
}

/* Sanity checks */
ASSERT(_kernel_size < 500M, "Kernel too large!")
ASSERT(_text_start >= KERNEL_VMA, "Text section not in higher half!")
ASSERT(_bss_start >= _data_start, "BSS overlaps data!")
ASSERT(_heap_start >= _bss_end, "Heap overlaps BSS!")
ASSERT(_stacks_start >= _heap_end, "Stacks overlap heap!")
ASSERT(_vectors_start % 2048 == 0, "Exception vectors not 2K aligned!")

/* Export symbols for kernel use */
PROVIDE(_kernel_virtual_start = KERNEL_VMA);
PROVIDE(_kernel_virtual_end = _kernel_end);
PROVIDE(_kernel_physical_start = KERNEL_LMA);
PROVIDE(_kernel_physical_end = _kernel_phys_end);

/* ARM64 specific symbols */
PROVIDE(_exception_vectors = _vectors_start);
PROVIDE(_cpu_stacks_start = _stacks_start);
PROVIDE(_cpu_stacks_end = _stacks_end);
PROVIDE(_percpu_data_start = _percpu_start);
PROVIDE(_percpu_data_end = _percpu_end);