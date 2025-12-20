#!/bin/bash
#===============================================================================
# BUILD ALL TESTS - Direct Compilation Script
#===============================================================================
# Builds all test binaries directly without relying on Makefile targets
# Usage: ./scripts/build_tests.sh [clean]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

echo "════════════════════════════════════════════════════════════"
echo "  Building HFT Test Binaries"
echo "════════════════════════════════════════════════════════════"

# Clean if requested
if [[ "$1" == "clean" ]]; then
    echo "[CLEAN] Removing build directory..."
    rm -rf build
fi

# Create build directory
mkdir -p build

# Compiler settings
CXX="c++"
CXXFLAGS="-std=c++20 -Wall -Wextra -O2 -pthread"
INCLUDES="-I. \
  -I$(brew --prefix boost)/include \
  -I$(brew --prefix openssl@3)/include \
  -I$(brew --prefix nlohmann-json)/include"
LIBS="-L$(brew --prefix openssl@3)/lib \
  -lssl -lcrypto -lcurl -lpthread"

echo ""
echo "[1/2] Building test_multi_exchange_l2..."
$CXX $CXXFLAGS $INCLUDES \
  test_scripts/test_multi_exchange_l2.cpp \
  $LIBS \
  -o build/test_multi_exchange_l2
echo "      ✓ Built: build/test_multi_exchange_l2"

echo ""
echo "[2/3] Building test_integrated_pipeline..."
$CXX $CXXFLAGS $INCLUDES \
  test_scripts/test_integrated_pipeline.cpp \
  $LIBS -lsqlite3 \
  -o build/test_integrated_pipeline
echo "      ✓ Built: build/test_integrated_pipeline"

echo ""
echo "[3/3] Building test_funding_arb_engine..."
$CXX $CXXFLAGS $INCLUDES \
  test_scripts/test_funding_arb_engine.cpp \
  $LIBS -lsqlite3 \
  -o build/test_funding_arb_engine
echo "      ✓ Built: build/test_funding_arb_engine"

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  ✓ All test binaries built successfully"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "Run tests with:"
echo "  ./build/test_multi_exchange_l2 [exchange]"
echo "  ./build/test_integrated_pipeline [runtime_seconds]"
echo "  ./build/test_funding_arb_engine [runtime_seconds]"
echo ""
