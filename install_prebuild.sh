#!/bin/bash

# ScreamALSA Installer
# Installs an existing snd-screamalsa.ko from the current directory into the system

set -e

# Output colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging helpers
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Root check
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (sudo)"
        exit 1
    fi
}

# Ensure driver file exists in current directory
ensure_driver_present() {
    if [[ ! -f "snd-screamalsa.ko" ]]; then
        log_error "snd-screamalsa.ko not found in the current directory"
        log_info "Please place the built module file here and run again"
        exit 1
    fi
}

# Install module into the system (no build)
install_module() {
    log_info "Installing snd-screamalsa.ko into the system..."

    # Create directory for the module
    local module_dir="/lib/modules/$(uname -r)/extra"
    mkdir -p "$module_dir"

    # Copy module
    cp -f snd-screamalsa.ko "$module_dir/"

    # Update module dependencies
    depmod -a

    # Create config file for autoload
    local modprobe_conf="/etc/modprobe.d/screamalsa.conf"
    mkdir -p "$(dirname "$modprobe_conf")"
    cat > "$modprobe_conf" << EOF
# ScreamALSA Driver Configuration
# Automatic loading of ScreamALSA driver
options snd-screamalsa ip_addr_str=192.168.1.77  port=4011 protocol_str=udp
EOF

    # Add module to autoload list
    local modules_load="/etc/modules-load.d/screamalsa.conf"
    mkdir -p "$(dirname "$modules_load")"
    echo "snd-screamalsa" > "$modules_load"
    log_info "Configuration: $modprobe_conf"
    log_info "Autoload: $modules_load"
}

# Main
check_root
ensure_driver_present
install_module
modprobe snd-screamalsa
log_success "Installation completed."


