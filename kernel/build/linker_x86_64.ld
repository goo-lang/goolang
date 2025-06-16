/* Goo Kernel Linker Script for x86_64 */

ENTRY(_start)

/* Define the kernel's virtual and physical addresses */
KERNEL_VMA = 0xFFFFFFFF80000000;  /* Virtual address (-2GB) */
KERNEL_LMA = 0x0000000000100000;  /* Physical address (1MB) */

/* Memory layout */
MEMORY
{
    /* Low memory for boot code and data */
    low_mem (rwx) : ORIGIN = 0x00000000, LENGTH = 1M
    
    /* Kernel memory */
    kernel (rwx) : ORIGIN = 1M, LENGTH = 510M
    
    /* High memory for user space */
    high_mem (rwx) : ORIGIN = 511M, LENGTH = 3G
}

SECTIONS
{
    /* Boot section (identity mapped) */
    .boot : AT(0x00100000)
    {
        KEEP(*(.boot.text))
        KEEP(*(.boot.data))
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
        _text_end = .;
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
    
    /* Exception handling tables */
    .eh_frame ALIGN(8) : AT(ADDR(.eh_frame) - KERNEL_VMA + KERNEL_LMA)
    {
        _eh_frame_start = .;
        KEEP(*(.eh_frame))
        _eh_frame_end = .;
    } >kernel
    
    .eh_frame_hdr ALIGN(8) : AT(ADDR(.eh_frame_hdr) - KERNEL_VMA + KERNEL_LMA)
    {
        _eh_frame_hdr_start = .;
        *(.eh_frame_hdr)
        _eh_frame_hdr_end = .;
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
        . = ALIGN(4K);
        _bss_end = .;
    } >kernel
    
    /* Kernel heap (reserve space) */
    .heap ALIGN(4K) : AT(ADDR(.heap) - KERNEL_VMA + KERNEL_LMA)
    {
        _heap_start = .;
        . += 16M;  /* 16MB heap */
        _heap_end = .;
    } >kernel
    
    /* Kernel stack (reserve space) */
    .stack ALIGN(4K) : AT(ADDR(.stack) - KERNEL_VMA + KERNEL_LMA)
    {
        _stack_bottom = .;
        . += 64K;  /* 64KB stack */
        _stack_top = .;
    } >kernel
    
    /* Page tables (reserve space) */
    .page_tables ALIGN(4K) : AT(ADDR(.page_tables) - KERNEL_VMA + KERNEL_LMA)
    {
        _page_tables_start = .;
        . += 1M;  /* 1MB for page tables */
        _page_tables_end = .;
    } >kernel
    
    /* Kernel end marker */
    _kernel_start = KERNEL_VMA;
    _kernel_end = .;
    _kernel_size = _kernel_end - _kernel_start;
    
    /* Physical addresses */
    _kernel_phys_start = KERNEL_LMA;
    _kernel_phys_end = _kernel_phys_start + _kernel_size;
    
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
    
    /* Symbol tables (not loaded) */
    .symtab 0 : { *(.symtab) }
    .strtab 0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }
    
    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gcc_except_table*)
    }
}

/* Sanity checks */
ASSERT(_kernel_size < 510M, "Kernel too large!")
ASSERT(_text_start >= KERNEL_VMA, "Text section not in higher half!")
ASSERT(_bss_start >= _data_start, "BSS overlaps data!")
ASSERT(_heap_start >= _bss_end, "Heap overlaps BSS!")
ASSERT(_stack_bottom >= _heap_end, "Stack overlaps heap!")

/* Export symbols for kernel use */
PROVIDE(_kernel_virtual_start = KERNEL_VMA);
PROVIDE(_kernel_virtual_end = _kernel_end);
PROVIDE(_kernel_physical_start = KERNEL_LMA);
PROVIDE(_kernel_physical_end = _kernel_phys_end);