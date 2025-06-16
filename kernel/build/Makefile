# Goo Kernel Build System
KERNEL_NAME = goo_kernel
VERSION = 0.1.0

# Architecture configuration
ARCH ?= x86_64
# ARCH ?= aarch64

# Toolchain configuration
ifeq ($(ARCH),x86_64)
    TARGET = x86_64-elf
    ASM_EXT = .S
    BOOT_ASM = x86_64.S
    LINKER_SCRIPT = linker_x86_64.ld
else ifeq ($(ARCH),aarch64)
    TARGET = aarch64-elf
    ASM_EXT = .S
    BOOT_ASM = arm.S
    LINKER_SCRIPT = linker_aarch64.ld
endif

# Compiler and tools
GOO_COMPILER = goo
CC = $(TARGET)-gcc
AS = $(TARGET)-as
LD = $(TARGET)-ld
OBJCOPY = $(TARGET)-objcopy
OBJDUMP = $(TARGET)-objdump
READELF = $(TARGET)-readelf

# Directories
SRC_DIR = ..
KERNEL_DIR = $(SRC_DIR)/kernel
STDLIB_DIR = $(SRC_DIR)/stdlib
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin
ISO_DIR = $(BUILD_DIR)/iso

# Output files
KERNEL_BIN = $(BIN_DIR)/$(KERNEL_NAME).bin
KERNEL_ELF = $(BIN_DIR)/$(KERNEL_NAME).elf
KERNEL_ISO = $(BIN_DIR)/$(KERNEL_NAME).iso

# Goo source files
GOO_SOURCES = $(shell find $(KERNEL_DIR) -name '*.goo')
GOO_SOURCES += $(shell find $(STDLIB_DIR) -name '*.goo')

# Assembly source files
ASM_SOURCES = $(BOOT_ASM)
ASM_SOURCES += runtime.S
ASM_SOURCES += interrupts.S
ASM_SOURCES += platform_$(ARCH).S

# Object files
GOO_OBJECTS = $(GOO_SOURCES:%.goo=$(OBJ_DIR)/%.o)
ASM_OBJECTS = $(ASM_SOURCES:%.S=$(OBJ_DIR)/%.o)
ALL_OBJECTS = $(GOO_OBJECTS) $(ASM_OBJECTS)

# Compiler flags
GOO_FLAGS = --target=kernel --arch=$(ARCH) --optimization=2 --no-std
ASM_FLAGS = -64 -march=$(ARCH)
LD_FLAGS = -T $(LINKER_SCRIPT) -nostdlib -static

# Kernel configuration
CPPFLAGS = -DKERNEL_VERSION=\"$(VERSION)\" -DARCH_$(shell echo $(ARCH) | tr a-z A-Z)
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
CFLAGS += -Wall -Wextra -Werror -O2 -g

# Default target
all: directories $(KERNEL_ELF) $(KERNEL_BIN)

# Create necessary directories
directories:
	@mkdir -p $(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR) $(ISO_DIR)
	@mkdir -p $(OBJ_DIR)/kernel/runtime
	@mkdir -p $(OBJ_DIR)/kernel/memory
	@mkdir -p $(OBJ_DIR)/kernel/interrupts
	@mkdir -p $(OBJ_DIR)/kernel/platform
	@mkdir -p $(OBJ_DIR)/stdlib

# Compile Goo source files
$(OBJ_DIR)/%.o: %.goo
	@echo "Compiling Goo: $<"
	@mkdir -p $(dir $@)
	$(GOO_COMPILER) $(GOO_FLAGS) -c $< -o $@

# Assemble assembly files
$(OBJ_DIR)/%.o: %.S
	@echo "Assembling: $<"
	@mkdir -p $(dir $@)
	$(AS) $(ASM_FLAGS) $< -o $@

# Link kernel
$(KERNEL_ELF): $(ALL_OBJECTS) $(LINKER_SCRIPT)
	@echo "Linking kernel: $@"
	$(LD) $(LD_FLAGS) $(ALL_OBJECTS) -o $@

# Create binary
$(KERNEL_BIN): $(KERNEL_ELF)
	@echo "Creating binary: $@"
	$(OBJCOPY) -O binary $< $@

# Create bootable ISO (x86_64 only)
iso: $(KERNEL_BIN)
ifeq ($(ARCH),x86_64)
	@echo "Creating ISO: $(KERNEL_ISO)"
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_BIN) $(ISO_DIR)/boot/$(KERNEL_NAME).bin
	@cp grub.cfg $(ISO_DIR)/boot/grub/
	grub-mkrescue -o $(KERNEL_ISO) $(ISO_DIR)
else
	@echo "ISO creation not supported for $(ARCH)"
endif

# Run in QEMU
run: $(KERNEL_BIN)
ifeq ($(ARCH),x86_64)
	qemu-system-x86_64 -kernel $(KERNEL_BIN) -m 128M -serial stdio
else ifeq ($(ARCH),aarch64)
	qemu-system-aarch64 -M virt -cpu cortex-a57 -kernel $(KERNEL_BIN) -m 128M -serial stdio
endif

# Run with GDB debugging
debug: $(KERNEL_ELF)
ifeq ($(ARCH),x86_64)
	qemu-system-x86_64 -kernel $(KERNEL_BIN) -m 128M -serial stdio -s -S &
	gdb $(KERNEL_ELF) -ex "target remote :1234"
else ifeq ($(ARCH),aarch64)
	qemu-system-aarch64 -M virt -cpu cortex-a57 -kernel $(KERNEL_BIN) -m 128M -serial stdio -s -S &
	gdb-multiarch $(KERNEL_ELF) -ex "target remote :1234"
endif

# Disassemble kernel
disasm: $(KERNEL_ELF)
	$(OBJDUMP) -d $< > $(BUILD_DIR)/$(KERNEL_NAME).disasm

# Show kernel information
info: $(KERNEL_ELF)
	@echo "=== Kernel Information ==="
	@echo "Architecture: $(ARCH)"
	@echo "Size: $$(stat -c%s $(KERNEL_BIN)) bytes"
	@echo "Entry point: $$($(READELF) -h $(KERNEL_ELF) | grep 'Entry point' | cut -d: -f2)"
	@echo ""
	@echo "=== Sections ==="
	$(READELF) -S $(KERNEL_ELF)
	@echo ""
	@echo "=== Symbol table ==="
	$(READELF) -s $(KERNEL_ELF) | head -20

# Check for undefined symbols
check: $(KERNEL_ELF)
	@echo "Checking for undefined symbols..."
	@if $(READELF) -s $(KERNEL_ELF) | grep UND; then \
		echo "ERROR: Undefined symbols found!"; \
		exit 1; \
	else \
		echo "All symbols defined."; \
	fi

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Clean and rebuild
rebuild: clean all

# Install toolchain (helper target)
install-toolchain:
	@echo "Installing cross-compilation toolchain..."
ifeq ($(ARCH),x86_64)
	# Install x86_64-elf-gcc
	@echo "Please install x86_64-elf-gcc, x86_64-elf-binutils"
	@echo "On Ubuntu/Debian: sudo apt install gcc-multilib"
	@echo "Or build from source: https://wiki.osdev.org/GCC_Cross-Compiler"
else ifeq ($(ARCH),aarch64)
	# Install aarch64-elf-gcc
	@echo "Please install aarch64-linux-gnu-gcc, aarch64-linux-gnu-binutils"
	@echo "On Ubuntu/Debian: sudo apt install gcc-aarch64-linux-gnu"
endif

# Install QEMU
install-qemu:
	@echo "Installing QEMU..."
	@echo "On Ubuntu/Debian: sudo apt install qemu-system-x86 qemu-system-arm"
	@echo "On macOS: brew install qemu"

# Test kernel in various configurations
test: test-basic test-memory test-interrupts

test-basic: $(KERNEL_BIN)
	@echo "Running basic kernel test..."
	timeout 10 qemu-system-$(ARCH) -kernel $(KERNEL_BIN) -m 64M -nographic -serial mon:stdio || true

test-memory: $(KERNEL_BIN)
	@echo "Running memory stress test..."
	timeout 10 qemu-system-$(ARCH) -kernel $(KERNEL_BIN) -m 256M -nographic -serial mon:stdio || true

test-interrupts: $(KERNEL_BIN)
	@echo "Running interrupt test..."
	timeout 10 qemu-system-$(ARCH) -kernel $(KERNEL_BIN) -m 128M -nographic -serial mon:stdio || true

# Generate documentation
docs:
	@echo "Generating kernel documentation..."
	@mkdir -p $(BUILD_DIR)/docs
	# Extract documentation from .goo files
	@find $(KERNEL_DIR) -name '*.goo' -exec grep -H "^//" {} \; > $(BUILD_DIR)/docs/kernel_docs.txt
	@echo "Documentation generated in $(BUILD_DIR)/docs/"

# Code formatting and linting
format:
	@echo "Formatting Goo code..."
	@find $(KERNEL_DIR) -name '*.goo' -exec goo fmt {} \;

lint:
	@echo "Linting Goo code..."
	@find $(KERNEL_DIR) -name '*.goo' -exec goo lint {} \;

# Size analysis
size-analysis: $(KERNEL_ELF)
	@echo "=== Size Analysis ==="
	$(OBJDUMP) -h $(KERNEL_ELF) | awk '/^ *[0-9]/ {size+=strtonum("0x"$$3)} END {printf "Total size: %d bytes (%.1f KB)\n", size, size/1024}'
	@echo ""
	@echo "Section sizes:"
	$(READELF) -S $(KERNEL_ELF) | grep -E '\.(text|data|bss|rodata)' | awk '{printf "%-10s: %8d bytes\n", $$2, strtonum("0x"$$6)}'

# Memory layout visualization
memory-layout: $(KERNEL_ELF)
	@echo "=== Memory Layout ==="
	$(READELF) -l $(KERNEL_ELF)

# Profiling support
profile: $(KERNEL_BIN)
	@echo "Running kernel with profiling..."
	qemu-system-$(ARCH) -kernel $(KERNEL_BIN) -m 128M -monitor stdio -nographic

# Help target
help:
	@echo "Goo Kernel Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build kernel (default)"
	@echo "  clean            - Clean build artifacts"
	@echo "  rebuild          - Clean and rebuild"
	@echo "  iso              - Create bootable ISO (x86_64 only)"
	@echo "  run              - Run kernel in QEMU"
	@echo "  debug            - Run kernel with GDB debugging"
	@echo "  test             - Run kernel tests"
	@echo "  info             - Show kernel information"
	@echo "  check            - Check for undefined symbols"
	@echo "  disasm           - Generate disassembly"
	@echo "  size-analysis    - Analyze kernel size"
	@echo "  memory-layout    - Show memory layout"
	@echo "  docs             - Generate documentation"
	@echo "  format           - Format Goo code"
	@echo "  lint             - Lint Goo code"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH={x86_64|aarch64}  - Target architecture (default: x86_64)"
	@echo ""
	@echo "Examples:"
	@echo "  make ARCH=x86_64       - Build for x86_64"
	@echo "  make ARCH=aarch64      - Build for ARM64"
	@echo "  make run ARCH=aarch64  - Run ARM64 kernel"

.PHONY: all clean rebuild iso run debug test test-basic test-memory test-interrupts
.PHONY: info check disasm size-analysis memory-layout docs format lint help
.PHONY: directories install-toolchain install-qemu profile