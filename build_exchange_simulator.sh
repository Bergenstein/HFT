#!/bin/bash
# Build script for Exchange Simulator (TODO #7)

set -e  # Exit on error

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║     Exchange Simulator Build Script                           ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Detect OS
OS=$(uname -s)
echo "Detected OS: $OS"

# Set compiler flags based on OS
if [ "$OS" == "Darwin" ]; then
    echo "Using macOS-specific settings..."
    INCLUDE_PATH="/opt/homebrew/include"
    LIB_PATH="/opt/homebrew/lib"
    
    # Check if Homebrew paths exist
    if [ ! -d "$INCLUDE_PATH" ]; then
        echo -e "${RED}ERROR: Homebrew include path not found at $INCLUDE_PATH${NC}"
        echo "Please install dependencies with: brew install boost websocketpp nlohmann-json"
        exit 1
    fi
else
    echo "Using Linux-specific settings..."
    INCLUDE_PATH="/usr/local/include"
    LIB_PATH="/usr/local/lib"
fi

# Compiler settings
CXX="g++"
CXXFLAGS="-std=c++17 -O3 -pthread -Wall -Wextra"
INCLUDES="-I. -I$INCLUDE_PATH"
LIBS="-L$LIB_PATH -lboost_system -lwebsocketpp -lpthread"

# Source files
SRC="run/test_exchange_simulator.cpp"
OUT="test_exchange_simulator"

echo ""
echo "Compiler: $CXX"
echo "Flags: $CXXFLAGS"
echo "Includes: $INCLUDES"
echo "Libraries: $LIBS"
echo "Source: $SRC"
echo "Output: $OUT"
echo ""

# Check if source file exists
if [ ! -f "$SRC" ]; then
    echo -e "${RED}ERROR: Source file not found: $SRC${NC}"
    exit 1
fi

# Check dependencies
echo "Checking dependencies..."

# Check for nlohmann/json
if [ ! -f "$INCLUDE_PATH/nlohmann/json.hpp" ]; then
    echo -e "${YELLOW}WARNING: nlohmann/json not found${NC}"
    echo "Install with: brew install nlohmann-json (macOS) or apt-get install nlohmann-json3-dev (Linux)"
fi

# Check for websocketpp
if [ ! -d "$INCLUDE_PATH/websocketpp" ]; then
    echo -e "${YELLOW}WARNING: websocketpp not found${NC}"
    echo "Install with: brew install websocketpp (macOS) or apt-get install libwebsocketpp-dev (Linux)"
fi

# Check for boost
if [ ! -d "$INCLUDE_PATH/boost" ]; then
    echo -e "${YELLOW}WARNING: boost not found${NC}"
    echo "Install with: brew install boost (macOS) or apt-get install libboost-all-dev (Linux)"
fi

echo ""
echo "Building exchange simulator..."
echo "Command: $CXX $CXXFLAGS $INCLUDES $SRC -o $OUT $LIBS"
echo ""

# Build
if $CXX $CXXFLAGS $INCLUDES $SRC -o $OUT $LIBS; then
    echo ""
    echo -e "${GREEN}✓ Build successful!${NC}"
    echo ""
    echo "Run the simulator with:"
    echo "  ./$OUT"
    echo ""
    echo "The WebSocket server will start on: ws://localhost:9001"
    echo ""
else
    echo ""
    echo -e "${RED}✗ Build failed${NC}"
    echo ""
    echo "Common issues:"
    echo "1. Missing dependencies - install with:"
    echo "   macOS:   brew install boost websocketpp nlohmann-json"
    echo "   Ubuntu:  sudo apt-get install libboost-all-dev libwebsocketpp-dev nlohmann-json3-dev"
    echo ""
    echo "2. Compiler not found - install with:"
    echo "   macOS:   xcode-select --install"
    echo "   Ubuntu:  sudo apt-get install build-essential"
    echo ""
    exit 1
fi
