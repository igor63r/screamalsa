# ScreamALSA Driver Makefile
# Universal support for Linux kernels 4.x - 6.x

# Get kernel version
KERNEL_VERSION := $(shell uname -r)
KERNEL_MAJOR := $(shell echo $(KERNEL_VERSION) | cut -d. -f1)
KERNEL_MINOR := $(shell echo $(KERNEL_VERSION) | cut -d. -f2)

# Module name
MODULE_NAME = snd-screamalsa
MODULE_FILE = $(MODULE_NAME).ko

# Source files
SRCS = snd-screamalsa.c

# Kernel build system
KERNEL_SRC ?= /lib/modules/$(KERNEL_VERSION)/build
KERNEL_OBJ ?= $(KERNEL_SRC)

# Compiler flags
EXTRA_CFLAGS += -Wall -Wextra -Wno-unused-parameter
EXTRA_CFLAGS += -DDEBUG

# Simplified kernel compatibility flags
EXTRA_CFLAGS += -DUSE_MANAGED_BUFFER=1
EXTRA_CFLAGS += -DUSE_KERNEL_SENDMSG=1

# Default target
obj-m := $(MODULE_NAME).o

# Build targets
.PHONY: all clean install uninstall load unload test help

all: $(MODULE_FILE)

$(MODULE_FILE): $(SRCS)
	@echo "Building ScreamALSA driver for kernel $(KERNEL_VERSION) ($(KERNEL_MAJOR).$(KERNEL_MINOR))"
	@echo "Kernel source: $(KERNEL_SRC)"
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules
	@echo "Build completed successfully"

clean:
	@echo "Cleaning build files..."
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
	rm -f $(MODULE_FILE) *.mod.c *.mod.o *.o modules.order Module.symvers
	rm -rf .tmp_versions/

# Installation targets
install: $(MODULE_FILE)
	@echo "Installing ScreamALSA driver..."
	@if [ ! -d /lib/modules/$(KERNEL_VERSION)/extra ]; then \
		mkdir -p /lib/modules/$(KERNEL_VERSION)/extra; \
	fi
	cp $(MODULE_FILE) /lib/modules/$(KERNEL_VERSION)/extra/
	depmod -a
	@echo "Driver installed successfully"

uninstall:
	@echo "Uninstalling ScreamALSA driver..."
	rm -f /lib/modules/$(KERNEL_VERSION)/extra/$(MODULE_FILE)
	depmod -a
	@echo "Driver uninstalled successfully"

# Module management
load: $(MODULE_FILE)
	@echo "Loading ScreamALSA driver..."
	@if lsmod | grep -q snd_screamalsa; then \
		echo "Driver already loaded, unloading first..."; \
		$(MAKE) unload; \
	fi
	insmod $(MODULE_FILE)
	@echo "Driver loaded successfully"

unload:
	@echo "Unloading ScreamALSA driver..."
	@if lsmod | grep -q snd_screamalsa; then \
		rmmod snd-screamalsa; \
		echo "Driver unloaded successfully"; \
	else \
		echo "Driver not loaded"; \
	fi

# Testing
test: load
	@echo "Testing ScreamALSA driver..."
	@if lsmod | grep -q snd_screamalsa; then \
		echo "✓ Driver loaded successfully"; \
	else \
		echo "✗ Driver failed to load"; \
		exit 1; \
	fi
	@if aplay -l 2>/dev/null | grep -q ScreamALSA; then \
		echo "✓ Sound card detected"; \
	else \
		echo "✗ Sound card not detected"; \
		exit 1; \
	fi
	@echo "Test completed successfully"

# Kernel compatibility check
check-kernel:
	@echo "Checking kernel compatibility..."
	@echo "Kernel version: $(KERNEL_VERSION)"
	@echo "Kernel major: $(KERNEL_MAJOR)"
	@echo "Kernel minor: $(KERNEL_MINOR)"
	@if [ $(KERNEL_MAJOR) -lt 4 ]; then \
		echo "✗ Unsupported kernel version. Requires Linux 4.x or newer."; \
		exit 1; \
	else \
		echo "✓ Kernel version $(KERNEL_VERSION) should be compatible"; \
	fi

# Build with kernel compatibility check
build: check-kernel all

# Help
help:
	@echo "ScreamALSA Driver Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the driver module"
	@echo "  clean      - Clean build files"
	@echo "  install    - Install driver to system"
	@echo "  uninstall  - Remove driver from system"
	@echo "  load       - Load driver module"
	@echo "  unload     - Unload driver module"
	@echo "  test       - Test driver functionality"
	@echo "  check-kernel - Check kernel compatibility"
	@echo "  build      - Check kernel and build driver"
	@echo "  help       - Show this help"
	@echo ""
	@echo "Environment variables:"
	@echo "  KERNEL_SRC - Kernel source directory (default: /lib/modules/\$(uname -r)/build)"
	@echo "  KERNEL_OBJ - Kernel object directory (default: same as KERNEL_SRC)"
	@echo ""
	@echo "Examples:"
	@echo "  make build                    # Build with kernel check"
	@echo "  make install                  # Install driver"
	@echo "  make load                     # Load driver"
	@echo "  make test                     # Test driver"
	@echo "  make KERNEL_SRC=/path/to/src # Build with custom kernel source"

# Debug information
debug:
	@echo "Debug information:"
	@echo "  Kernel version: $(KERNEL_VERSION)"
	@echo "  Kernel major: $(KERNEL_MAJOR)"
	@echo "  Kernel minor: $(KERNEL_MINOR)"
	@echo "  Kernel source: $(KERNEL_SRC)"
	@echo "  Module name: $(MODULE_NAME)"
	@echo "  Module file: $(MODULE_FILE)"
	@echo "  Extra CFLAGS: $(EXTRA_CFLAGS)"
	@echo "  Source files: $(SRCS)"

