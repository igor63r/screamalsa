#!/bin/bash

set -Eeuo pipefail

echo "ScreamALSA Build"
echo "===================="

#----- Helpers ---------------------------------------------------------------#
have_cmd() { command -v "$1" &>/dev/null; }

OS_ID="unknown"
OS_LIKE=""
OS_NAME="$(uname -s)"
PKG_MGR="unknown"

detect_os() {
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        OS_ID="${ID:-unknown}"
        OS_LIKE="${ID_LIKE:-}"
        OS_NAME="${PRETTY_NAME:-$OS_ID}"
    fi

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

# On Arch-based, find correct header pkg (linux-headers, linux-lts-headers, etc.)
arch_header_pkg() {
    local pkgbase=""
    pkgbase="$(cat "/lib/modules/$(uname -r)/pkgbase" 2>/dev/null || true)"
    if [[ -n "$pkgbase" ]]; then
        echo "${pkgbase}-headers"
    else
        echo "linux-headers"
    fi
}

print_header_hint() {
    local kv="$(uname -r)"
    echo "→ How to install kernel headers and build tools on your system:"
    case "$PKG_MGR" in
        pacman)
            local hpkg; hpkg="$(arch_header_pkg)"
            echo "  - Arch/Manjaro/EndeavourOS: sudo pacman -S --needed base-devel ${hpkg}"
            ;;
        apt)
            echo "  - Debian/Ubuntu: sudo apt-get update && sudo apt-get install -y build-essential linux-headers-${kv}"
            ;;
        dnf)
            echo "  - Fedora/RHEL-like: sudo dnf groupinstall -y 'Development Tools'"
            echo "                       sudo dnf install -y kernel-devel-${kv} kernel-headers-${kv}"
            ;;
        yum)
            echo "  - CentOS/RHEL (yum): sudo yum groupinstall -y 'Development Tools'"
            echo "                       sudo yum install -y kernel-devel-${kv} kernel-headers-${kv}"
            ;;
        zypper)
            echo "  - openSUSE/SLE: sudo zypper install -t pattern devel_kernel"
            echo "                  sudo zypper install kernel-devel make gcc"
            ;;
        xbps)
            echo "  - Void Linux: sudo xbps-install -S base-devel linux-headers"
            ;;
        apk)
            echo "  - Alpine: sudo apk add --no-cache build-base"
            echo "            For kernel headers, install the dev package for your kernel variant,"
            echo "            e.g.: sudo apk add --no-cache linux-lts-dev (or linux-virt-dev/linux-vanilla-dev)"
            ;;
        emerge)
            echo "  - Gentoo: sudo emerge --ask sys-devel/gcc sys-devel/make virtual/linux-sources"
            echo "            Make sure /usr/src/linux matches the running kernel and run:"
            echo "            sudo make -C /usr/src/linux prepare modules_prepare"
            ;;
        nix)
            echo "  - NixOS: use a build environment, for example:"
            echo "            nix-shell -p gcc gnumake linuxPackages.kernel.dev --run 'make'"
            ;;
        *)
            echo "  - Could not detect a package manager. Install kernel headers and build tools manually."
            ;;
    esac
}

print_build_tools_hint() {
    case "$PKG_MGR" in
        pacman) echo "  Hint: sudo pacman -S --needed base-devel" ;;
        apt)    echo "  Hint: sudo apt-get install -y build-essential" ;;
        dnf)    echo "  Hint: sudo dnf groupinstall -y 'Development Tools'" ;;
        yum)    echo "  Hint: sudo yum groupinstall -y 'Development Tools'" ;;
        zypper) echo "  Hint: sudo zypper install -t pattern devel_C_C++" ;;
        xbps)   echo "  Hint: sudo xbps-install -S base-devel" ;;
        apk)    echo "  Hint: sudo apk add --no-cache build-base" ;;
        emerge) echo "  Hint: sudo emerge --ask sys-devel/gcc sys-devel/make" ;;
        nix)    echo "  Hint: nix-shell -p gcc gnumake" ;;
        *)      echo "  Hint: install a compiler (gcc) and make for your system." ;;
    esac
}

#----- Start -----------------------------------------------------------------#

detect_os
echo "Detected OS: ${OS_NAME} (pkgmgr: ${PKG_MGR})"

# Check if we're in the right directory
if [[ ! -f "snd-screamalsa.c" ]]; then
    echo "Error: snd-screamalsa.c not found"
    echo "Please run this script from the ScreamALSA directory"
    exit 1
fi

# WSL note (modules are not supported there)
if grep -qi microsoft /proc/version 2>/dev/null; then
    echo "Note: You appear to be on WSL. Building kernel modules on WSL is typically not supported."
fi

# Check kernel version
echo "Kernel version: $(uname -r)"

# Check for required files
echo "Checking required files..."
required_files=("Makefile" "snd-screamalsa.c")
for file in "${required_files[@]}"; do
    if [[ -f "$file" ]]; then
        echo "✓ $file found"
    else
        echo "✗ $file missing"
        exit 1
    fi
done

# Check for kernel headers
echo "Checking kernel headers..."
if [[ -d "/lib/modules/$(uname -r)/build" ]]; then
    echo "✓ Kernel headers found"
else
    echo "✗ Kernel headers missing for $(uname -r)"
    print_header_hint
    exit 1
fi

# Check for build tools
echo "Checking build tools..."
if have_cmd gcc; then
    echo "✓ GCC found: $(gcc --version | head -n1)"
else
    echo "✗ GCC missing"
    print_build_tools_hint
    exit 1
fi

if have_cmd make; then
    echo "✓ Make found: $(make --version | head -n1)"
else
    echo "✗ Make missing"
    print_build_tools_hint
    exit 1
fi

# Try to build
echo "Attempting build..."
make clean > /dev/null 2>&1 || true

if make > build.log 2>&1; then
    echo "✓ Build successful"
    if [[ -f snd-screamalsa.ko ]]; then
        ls -la snd-screamalsa.ko
        if have_cmd modinfo; then
            modinfo snd-screamalsa.ko | head -5
        else
            echo "(modinfo not found; skipping module info output)"
        fi
    fi
else
    echo "✗ Build failed"
    echo "Build log:"
    cat build.log
    echo ""
    echo "Common solutions:"
    print_header_hint
    print_build_tools_hint
    exit 1
fi

echo ""
echo "Build completed successfully!"
