#!/bin/bash
#==============================================================================
# HFT System - Linux Dependency Installation Script
#==============================================================================
# This script installs all required dependencies for Ubuntu/Debian systems
# For other distributions, adjust package manager commands accordingly
#==============================================================================

set -e  # Exit on error

echo "=================================="
echo "HFT System Dependency Installer"
echo "=================================="
echo ""

# Detect Linux distribution
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    VER=$VERSION_ID
else
    echo "Cannot detect Linux distribution"
    exit 1
fi

echo "Detected OS: $OS $VER"
echo ""

# Check if running as root
if [ "$EUID" -eq 0 ]; then 
    SUDO=""
else
    SUDO="sudo"
fi

#==============================================================================
# UBUNTU/DEBIAN
#==============================================================================
if [[ "$OS" == "ubuntu" ]] || [[ "$OS" == "debian" ]]; then
    echo "Installing dependencies for Ubuntu/Debian..."
    echo ""
    
    # Update package list
    $SUDO apt-get update
    
    # Essential build tools
    echo "Installing build tools..."
    $SUDO apt-get install -y build-essential cmake git pkg-config
    
    # C++20 compiler (GCC 10+)
    echo "Installing C++20 compiler..."
    $SUDO apt-get install -y g++-11 gcc-11 || $SUDO apt-get install -y g++-10 gcc-10
    
    # Core libraries
    echo "Installing core libraries..."
    $SUDO apt-get install -y \
        libssl-dev \
        libcurl4-openssl-dev \
        libzmq3-dev \
        libboost-all-dev \
        libsqlite3-dev
    
    # simdjson (may need to build from source if not available)
    echo "Installing simdjson..."
    if ! $SUDO apt-get install -y libsimdjson-dev 2>/dev/null; then
        echo "simdjson not in repos, building from source..."
        cd /tmp
        git clone https://github.com/simdjson/simdjson.git
        cd simdjson
        mkdir -p build && cd build
        cmake ..
        make -j$(nproc)
        $SUDO make install
        cd /tmp && rm -rf simdjson
    fi
    
    # Protobuf and Abseil
    echo "Installing protobuf and abseil..."
    $SUDO apt-get install -y \
        libprotobuf-dev \
        protobuf-compiler \
        libabsl-dev
    
    # nlohmann-json
    echo "Installing nlohmann-json..."
    if ! $SUDO apt-get install -y nlohmann-json3-dev 2>/dev/null; then
        echo "Installing nlohmann-json from header-only source..."
        $SUDO mkdir -p /usr/local/include
        $SUDO wget -O /usr/local/include/json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    fi
    
    # WebSocket++ (header-only)
    echo "Installing websocketpp..."
    if ! $SUDO apt-get install -y libwebsocketpp-dev 2>/dev/null; then
        echo "Installing websocketpp from source..."
        cd /tmp
        git clone https://github.com/zaphoyd/websocketpp.git
        cd websocketpp
        $SUDO cp -r websocketpp /usr/local/include/
        cd /tmp && rm -rf websocketpp
    fi
    
    # cppzmq (header-only wrapper for ZeroMQ)
    echo "Installing cppzmq..."
    if ! $SUDO apt-get install -y libcppzmq-dev 2>/dev/null; then
        echo "Installing cppzmq from source..."
        cd /tmp
        git clone https://github.com/zeromq/cppzmq.git
        cd cppzmq
        $SUDO cp *.hpp /usr/local/include/
        cd /tmp && rm -rf cppzmq
    fi
    
    echo ""
    echo "✓ All dependencies installed successfully!"

#==============================================================================
# FEDORA/RHEL/CENTOS
#==============================================================================
elif [[ "$OS" == "fedora" ]] || [[ "$OS" == "rhel" ]] || [[ "$OS" == "centos" ]]; then
    echo "Installing dependencies for Fedora/RHEL/CentOS..."
    echo ""
    
    # Update package list
    $SUDO dnf update -y
    
    # Essential build tools
    echo "Installing build tools..."
    $SUDO dnf install -y gcc-c++ cmake git pkg-config
    
    # Core libraries
    echo "Installing core libraries..."
    $SUDO dnf install -y \
        openssl-devel \
        libcurl-devel \
        zeromq-devel \
        boost-devel \
        sqlite-devel \
        protobuf-devel \
        protobuf-compiler \
        abseil-cpp-devel
    
    # simdjson (build from source)
    echo "Installing simdjson..."
    cd /tmp
    git clone https://github.com/simdjson/simdjson.git
    cd simdjson
    mkdir -p build && cd build
    cmake ..
    make -j$(nproc)
    $SUDO make install
    cd /tmp && rm -rf simdjson
    
    # nlohmann-json
    echo "Installing nlohmann-json..."
    $SUDO dnf install -y json-devel || {
        $SUDO mkdir -p /usr/local/include
        $SUDO wget -O /usr/local/include/json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    }
    
    # WebSocket++ (header-only)
    echo "Installing websocketpp..."
    cd /tmp
    git clone https://github.com/zaphoyd/websocketpp.git
    cd websocketpp
    $SUDO cp -r websocketpp /usr/local/include/
    cd /tmp && rm -rf websocketpp
    
    # cppzmq (header-only)
    echo "Installing cppzmq..."
    cd /tmp
    git clone https://github.com/zeromq/cppzmq.git
    cd cppzmq
    $SUDO cp *.hpp /usr/local/include/
    cd /tmp && rm -rf cppzmq
    
    echo ""
    echo "✓ All dependencies installed successfully!"

#==============================================================================
# ARCH LINUX
#==============================================================================
elif [[ "$OS" == "arch" ]] || [[ "$OS" == "manjaro" ]]; then
    echo "Installing dependencies for Arch Linux..."
    echo ""
    
    $SUDO pacman -Syu --noconfirm
    $SUDO pacman -S --noconfirm \
        base-devel \
        cmake \
        git \
        boost \
        openssl \
        curl \
        zeromq \
        cppzmq \
        sqlite \
        protobuf \
        abseil-cpp \
        nlohmann-json \
        websocketpp \
        simdjson
    
    echo ""
    echo "✓ All dependencies installed successfully!"

else
    echo "Unsupported distribution: $OS"
    echo "Please install dependencies manually:"
    echo "  - g++ with C++20 support"
    echo "  - boost, openssl, curl, zeromq, protobuf, abseil"
    echo "  - simdjson, nlohmann-json, websocketpp, cppzmq"
    exit 1
fi

#==============================================================================
# UPDATE LINKER CACHE
#==============================================================================
echo ""
echo "Updating linker cache..."
$SUDO ldconfig

#==============================================================================
# VERIFY INSTALLATION
#==============================================================================
echo ""
echo "Verifying installation..."
echo ""

# Check for C++ compiler
if command -v g++ &> /dev/null; then
    echo "✓ g++ found: $(g++ --version | head -n1)"
else
    echo "✗ g++ not found"
fi

# Check for required headers
echo ""
echo "Checking headers..."
for header in "boost/version.hpp" "openssl/ssl.h" "zmq.hpp" "simdjson.h" "nlohmann/json.hpp" "websocketpp/version.hpp"; do
    if echo '#include <'$header'>' | g++ -x c++ -E - &>/dev/null; then
        echo "✓ $header found"
    else
        echo "✗ $header not found"
    fi
done

echo ""
echo "=================================="
echo "Installation complete!"
echo "=================================="
echo ""
echo "You can now build the system with:"
echo "  make -f Makefile.crossplatform tests"
echo ""
