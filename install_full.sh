#!/bin/bash

# ScreamALSA Driver Installer
# Automatic installation of the virtual sound card driver

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

# Detect package manager (multi-distro)
have_cmd() { command -v "$1" &>/dev/null; }

PKG_MGR="unknown"

detect_pkg_mgr() {
    if have_cmd pacman; then
        PKG_MGR="pacman"
    elif have_cmd apt-get; then
        PKG_MGR="apt"
    elif have_cmd dnf; then
        PKG_MGR="dnf"
    elif have_cmd yum; then
        PKG_MGR="yum"
    elif have_cmd zypper; then
        PKG_MGR="zypper"
    elif have_cmd apk; then
        PKG_MGR="apk"
    elif have_cmd xbps-install; then
        PKG_MGR="xbps"
    elif have_cmd emerge; then
        PKG_MGR="emerge"
    elif have_cmd nix-shell; then
        PKG_MGR="nix"
    else
        PKG_MGR="unknown"
    fi
}

# Arch-based: resolve correct headers package (linux-headers, linux-lts-headers, ...)
arch_header_pkg() {
    local pkgbase=""
    pkgbase="$(cat "/lib/modules/$(uname -r)/pkgbase" 2>/dev/null || true)"
    if [[ -n "$pkgbase" ]]; then
        echo "${pkgbase}-headers"
    else
        echo "linux-headers"
    fi
}

# Root check
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (sudo)"
        exit 1
    fi
}

# Dependencies check (multi-distro)
check_dependencies() {
    log_info "Checking dependencies..."
    detect_pkg_mgr
    
    local need_build_tools=0
    local install_pkgs=()
    
    # Build tools: gcc and make
    if ! have_cmd gcc || ! have_cmd make; then
        need_build_tools=1
    fi
    
    # Check kernel headers
    local headers_missing=0
    if [ ! -d "/usr/src/linux-headers-$(uname -r)" ] && [ ! -d "/lib/modules/$(uname -r)/build" ]; then
        headers_missing=1
    fi
    
    # Check ALSA dev
    if [ ! -f "/usr/include/alsa/asoundlib.h" ]; then
        case "$PKG_MGR" in
            apt)    install_pkgs+=("libasound2-dev") ;;
            pacman) install_pkgs+=("alsa-lib") ;;
            dnf|yum) install_pkgs+=("alsa-lib-devel") ;;
            zypper) install_pkgs+=("alsa-devel") ;;
            xbps)   install_pkgs+=("alsa-lib-devel") ;;
            apk)    install_pkgs+=("alsa-lib-dev") ;;
            emerge) ;; # handled via message below
            *)      ;; # unknown
        esac
    fi
    
    # Kernel headers per distro
    if [ "$headers_missing" -eq 1 ]; then
        case "$PKG_MGR" in
            apt)    install_pkgs+=("linux-headers-$(uname -r)") ;;
            pacman) install_pkgs+=("$(arch_header_pkg)") ;;
            dnf|yum) install_pkgs+=("kernel-devel-$(uname -r)" "kernel-headers-$(uname -r)") ;;
            zypper) install_pkgs+=("kernel-devel") ;;
            xbps)   install_pkgs+=("linux-headers") ;;
            apk)    log_warning "On Alpine, install the dev package for your kernel variant (e.g. linux-lts-dev)." ;;
            emerge) log_warning "On Gentoo, install virtual/linux-sources and prepare the kernel tree." ;;
            *)      log_warning "Unknown package manager. Install kernel headers manually." ;;
        esac
    fi

    # Install build tools per distro if needed
    if [ "$need_build_tools" -eq 1 ]; then
        case "$PKG_MGR" in
            pacman) log_info "Installing build tools (base-devel)..."; pacman -S --needed --noconfirm base-devel ;;
            apt)    log_info "Installing build tools (build-essential)..."; apt-get update && apt-get install -y build-essential ;;
            dnf)    log_info "Installing build tools (Development Tools group)..."; dnf -y groupinstall 'Development Tools' ;;
            yum)    log_info "Installing build tools (Development Tools group)..."; yum -y groupinstall 'Development Tools' ;;
            zypper) log_info "Installing build tools (devel_C_C++ pattern)..."; zypper -n install -t pattern devel_C_C++ ;;
            xbps)   log_info "Installing build tools (base-devel)..."; xbps-install -Sy base-devel ;;
            apk)    log_info "Installing build tools (build-base)..."; apk add --no-cache build-base ;;
            emerge) log_warning "On Gentoo, install sys-devel/gcc and sys-devel/make manually (emerge --ask)." ;;
            nix)    log_warning "On NixOS, use nix-shell -p gcc gnumake to provide build tools." ;;
            *)      log_warning "Unknown package manager. Install gcc and make manually." ;;
        esac
    fi

    # Install remaining packages collected in install_pkgs
    if [ ${#install_pkgs[@]} -ne 0 ]; then
        log_info "Installing additional packages: ${install_pkgs[*]}"
        case "$PKG_MGR" in
            apt)    apt-get update && apt-get install -y "${install_pkgs[@]}" ;;
            pacman) pacman -S --needed --noconfirm "${install_pkgs[@]}" ;;
            dnf)    dnf install -y "${install_pkgs[@]}" ;;
            yum)    yum install -y "${install_pkgs[@]}" ;;
            zypper) zypper -n install "${install_pkgs[@]}" ;;
            xbps)   xbps-install -Sy "${install_pkgs[@]}" ;;
            apk)    apk add --no-cache "${install_pkgs[@]}" ;;
            emerge) ;; # handled via messages
            nix)    ;; # advise nix-shell if needed
            *)      log_error "Could not detect a package manager. Install manually: ${install_pkgs[*]}"; exit 1 ;;
        esac
    fi
    
    log_success "All dependencies are installed"
}

# Build module
build_module() {
    log_info "Building kernel module via build_scream.sh..."
    
    if ! ./build_scream.sh; then
        log_error "Module build failed"
        exit 1
    fi
    
    log_success "Module built successfully"
}

# Install module into the system
install_module() {
    log_info "Installing module into the system..."
    
    # Create directory for the module
    local module_dir="/lib/modules/$(uname -r)/extra"
    mkdir -p "$module_dir"
    
    # Copy module
    cp snd-screamalsa.ko "$module_dir/"
    
    # Update module dependencies
    depmod -a
    
    # Create config file for autoload
    local modprobe_conf="/etc/modprobe.d/screamalsa.conf"
    mkdir -p "$(dirname "$modprobe_conf")"
    cat > "$modprobe_conf" << EOF
# ScreamALSA Driver Configuration
# Automatic loading of ScreamALSA driver
options snd-screamalsa ip_addr_str=192.168.85.1  port=4011 protocol_str=udp
EOF
    
    # Add module to autoload list
    local modules_load="/etc/modules-load.d/screamalsa.conf"
    mkdir -p "$(dirname "$modules_load")"
    echo "snd-screamalsa" > "$modules_load"
    
    log_success "Module installed into the system"
    log_info "Configuration saved to: $modprobe_conf"
    log_info "Autoload configured at: $modules_load"
}

# Load module
load_module() {
    log_info "Loading module..."
    
    # Check if module is already loaded
    if lsmod | grep -q "snd_screamalsa"; then
        log_warning "Module already loaded. Unloading first..."
        modprobe -r snd-screamalsa || true
    fi
    
    # Load module
    if ! modprobe snd-screamalsa; then
        log_error "Module load failed"
        exit 1
    fi
    
    log_success "Module loaded successfully"
}

# Unload module
unload_module() {
    log_info "Unloading module..."
    
    if lsmod | grep -q "snd_screamalsa"; then
        modprobe -r snd-screamalsa
        log_success "Module unloaded"
    else
        log_warning "Module was not loaded"
    fi
}

# Remove module from the system
remove_module() {
    log_info "Removing module from the system..."
    
    # Unload module if loaded
    unload_module
    
    # Remove configuration files
    rm -f "/etc/modprobe.d/screamalsa.conf"
    rm -f "/etc/modules-load.d/screamalsa.conf"
    
    # Remove module
    rm -f "/lib/modules/$(uname -r)/extra/snd-screamalsa.ko"
    
    # Refresh dependencies
    depmod -a
    
    log_success "Module completely removed from the system"
}

# ALSA setup
setup_alsa() {
    log_info "Configuring ALSA..."
    
    # Create ALSA configuration
    local asound_conf="/etc/asound.conf"
    if [ ! -f "$asound_conf" ]; then
        cat > "$asound_conf" << 'EOF'
# ScreamALSA Configuration
# Virtual sound card ScreamALSA

pcm.!default {
    type hw
    card ScreamALSA
    device 0
}

ctl.!default {
    type hw
    card ScreamALSA
}
EOF
        log_success "Created ALSA configuration file: $asound_conf"
    else
        log_warning "$asound_conf already exists. Check configuration manually."
    fi
}

# Test
test_module() {
    log_info "Testing module..."
    
    # Check if module is loaded
    if ! lsmod | grep -q "snd_screamalsa"; then
        log_error "Module is not loaded"
        return 1
    fi
    
    # Check that the sound card exists
    if ! aplay -l | grep -q "ScreamALSA"; then
        log_error "ScreamALSA sound card not found"
        return 1
    fi
    
    log_success "Module works correctly"
    log_info "Sound card: $(aplay -l | grep ScreamALSA)"
}

# Show status
show_status() {
    log_info "ScreamALSA status:"
    
    echo "  Module loaded: $(lsmod | grep -q "snd_screamalsa" && echo "Yes" || echo "No")"
    echo "  Sound card: $(aplay -l | grep -q "ScreamALSA" && echo "Present" || echo "Not present")"
    echo "  Configuration: $(ls -la /etc/modprobe.d/screamalsa.conf 2>/dev/null && echo "Installed" || echo "Not installed")"
    echo "  Autoload: $(ls -la /etc/modules-load.d/screamalsa.conf 2>/dev/null && echo "Configured" || echo "Not configured")"
}

# Help
show_help() {
    echo "ScreamALSA Driver Installer"
    echo ""
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  install    - Full installation (build + install + load)"
    echo "  build      - Build module only"
    echo "  load       - Load module"
    echo "  unload     - Unload module"
    echo "  remove     - Full removal from the system"
    echo "  status     - Show status"
    echo "  test       - Test the module"
    echo "  help       - Show this help"
    echo ""
    echo "Examples:"
    echo "  sudo $0 install    # Full install"
    echo "  sudo $0 remove     # Remove"
    echo "  $0 status          # Status (without sudo)"
}

# Main logic
main() {
    case "${1:-help}" in
        install)
            check_root
            # Optionally check dependencies
            # check_dependencies
            # Build via build_scream.sh
            build_module
            install_module
            load_module
            # Optionally set up ALSA
            # setup_alsa
            test_module
            log_success "Installation completed successfully!"
            log_info "The module will be automatically loaded at system boot"
            ;;
        build)
            check_root
            # Optionally check dependencies
            # check_dependencies
            build_module
            ;;
        load)
            check_root
            load_module
            ;;
        unload)
            check_root
            unload_module
            ;;
        remove)
            check_root
            remove_module
            ;;
        status)
            show_status
            ;;
        test)
            check_root
            test_module
            ;;
        help|*)
            show_help
            ;;
    esac
}

# Run main
main "$@"

