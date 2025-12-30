#!/bin/bash
#==============================================================================
# HFT System - Cross-Platform Test Suite
#==============================================================================
# Tests all core components to ensure Linux/macOS compatibility
#==============================================================================

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test results tracking
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

echo "=================================="
echo "HFT System Test Suite"
echo "=================================="
echo ""
echo "OS: $(uname -s)"
echo "Architecture: $(uname -m)"
echo "Date: $(date)"
echo ""

#==============================================================================
# Helper Functions
#==============================================================================

test_passed() {
    echo -e "${GREEN}✓ PASS${NC} - $1"
    ((PASSED_TESTS++))
    ((TOTAL_TESTS++))
}

test_failed() {
    echo -e "${RED}✗ FAIL${NC} - $1"
    echo -e "${RED}  Error: $2${NC}"
    ((FAILED_TESTS++))
    ((TOTAL_TESTS++))
}

test_section() {
    echo ""
    echo -e "${BLUE}================================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}================================================${NC}"
    echo ""
}

#==============================================================================
# Test 1: Lock-Free Queues
#==============================================================================

test_section "Test 1: Lock-Free Queues (SPSC & MPMC)"

if [ -f "build/test_lockfree_queues" ]; then
    if ./build/test_lockfree_queues > /tmp/test_lockfree.log 2>&1; then
        if grep -q "All tests passed" /tmp/test_lockfree.log; then
            test_passed "Lock-free queues working correctly"
        else
            test_failed "Lock-free queues" "Tests did not complete successfully"
            cat /tmp/test_lockfree.log
        fi
    else
        test_failed "Lock-free queues" "Test binary crashed"
        cat /tmp/test_lockfree.log
    fi
else
    test_failed "Lock-free queues" "Binary not found - run 'make -f Makefile.crossplatform tests'"
fi

#==============================================================================
# Test 2: Matching Engine
#==============================================================================

test_section "Test 2: Matching Engine"

if [ -f "build/test_matching_engine_simple" ]; then
    if ./build/test_matching_engine_simple > /tmp/test_matching.log 2>&1; then
        if grep -q "PASS" /tmp/test_matching.log || grep -q "SUCCESS" /tmp/test_matching.log; then
            test_passed "Matching engine working correctly"
        else
            test_failed "Matching engine" "Tests did not complete successfully"
            head -20 /tmp/test_matching.log
        fi
    else
        test_failed "Matching engine" "Test binary crashed"
        head -20 /tmp/test_matching.log
    fi
else
    test_failed "Matching engine" "Binary not found"
fi

#==============================================================================
# Test 3: System Pipeline
#==============================================================================

test_section "Test 3: System Pipeline (Core + Storage)"

if [ -f "build/test_system_pipeline" ]; then
    if timeout 10s ./build/test_system_pipeline > /tmp/test_pipeline.log 2>&1; then
        if grep -q "PASS" /tmp/test_pipeline.log || grep -q "SUCCESS" /tmp/test_pipeline.log; then
            test_passed "System pipeline working correctly"
        else
            test_failed "System pipeline" "Tests did not complete successfully"
            head -20 /tmp/test_pipeline.log
        fi
    else
        EXIT_CODE=$?
        if [ $EXIT_CODE -eq 124 ]; then
            test_failed "System pipeline" "Test timed out after 10s"
        else
            test_failed "System pipeline" "Test binary crashed (exit code: $EXIT_CODE)"
        fi
        head -20 /tmp/test_pipeline.log
    fi
else
    test_failed "System pipeline" "Binary not found"
fi

#==============================================================================
# Test 4: Funding Rates
#==============================================================================

test_section "Test 4: Funding Rates"

if [ -f "build/test_funding_rates_simple" ]; then
    if timeout 10s ./build/test_funding_rates_simple > /tmp/test_funding.log 2>&1; then
        if grep -q "PASS" /tmp/test_funding.log || grep -q "SUCCESS" /tmp/test_funding.log; then
            test_passed "Funding rates working correctly"
        else
            test_failed "Funding rates" "Tests did not complete successfully"
            head -20 /tmp/test_funding.log
        fi
    else
        EXIT_CODE=$?
        if [ $EXIT_CODE -eq 124 ]; then
            test_failed "Funding rates" "Test timed out after 10s"
        else
            test_failed "Funding rates" "Test binary crashed (exit code: $EXIT_CODE)"
        fi
        head -20 /tmp/test_funding.log
    fi
else
    test_failed "Funding rates" "Binary not found"
fi

#==============================================================================
# Test 5: ZeroMQ Pub/Sub
#==============================================================================

test_section "Test 5: ZeroMQ Pub/Sub Communication"

if [ -f "build/test_zmq_pubsub" ]; then
    if timeout 5s ./build/test_zmq_pubsub > /tmp/test_zmq.log 2>&1; then
        if grep -q "PASS" /tmp/test_zmq.log || grep -q "SUCCESS" /tmp/test_zmq.log; then
            test_passed "ZeroMQ pub/sub working correctly"
        else
            test_failed "ZeroMQ pub/sub" "Tests did not complete successfully"
            head -20 /tmp/test_zmq.log
        fi
    else
        EXIT_CODE=$?
        if [ $EXIT_CODE -eq 124 ]; then
            test_failed "ZeroMQ pub/sub" "Test timed out after 5s"
        else
            test_failed "ZeroMQ pub/sub" "Test binary crashed (exit code: $EXIT_CODE)"
        fi
        head -20 /tmp/test_zmq.log
    fi
else
    test_failed "ZeroMQ pub/sub" "Binary not found"
fi

#==============================================================================
# Test 6: Exchange Integration Tests
#==============================================================================

test_section "Test 6: Exchange Integration"

# Test GRVT
if [ -f "build/test_grvt_connection" ]; then
    echo -e "${YELLOW}Note: GRVT test requires network connection${NC}"
    test_passed "GRVT integration compiled"
else
    test_failed "GRVT integration" "Binary not found"
fi

# Test Coinbase
if [ -f "build/test_coinbase_l2" ]; then
    echo -e "${YELLOW}Note: Coinbase test requires network connection${NC}"
    test_passed "Coinbase L2 integration compiled"
else
    test_failed "Coinbase L2 integration" "Binary not found"
fi

#==============================================================================
# Test 7: Protobuf Compatibility
#==============================================================================

test_section "Test 7: Protobuf Compatibility"

if [ -f "proto/messages.pb.h" ] && [ -f "proto/messages.pb.cc" ]; then
    test_passed "Protobuf files exist"
    
    # Check if protobuf version is compatible
    PROTOC_VERSION=$(protoc --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | head -1)
    if [ -n "$PROTOC_VERSION" ]; then
        echo "  Protoc version: $PROTOC_VERSION"
        test_passed "Protobuf compiler found"
    else
        test_failed "Protobuf compiler" "protoc not found in PATH"
    fi
else
    test_failed "Protobuf files" "Generated files missing - run 'make -f Makefile.crossplatform proto'"
fi

#==============================================================================
# Test 8: Library Dependencies
#==============================================================================

test_section "Test 8: Library Dependencies"

check_library() {
    if ldconfig -p 2>/dev/null | grep -q "$1" || [ -f "/usr/lib/lib$1.so" ] || [ -f "/usr/local/lib/lib$1.so" ]; then
        test_passed "Library $1 found"
        return 0
    else
        test_failed "Library $1" "Not found in system"
        return 1
    fi
}

# Check core libraries (Linux specific - skip on macOS)
if [ "$(uname -s)" = "Linux" ]; then
    check_library "ssl"
    check_library "crypto"
    check_library "zmq"
    check_library "protobuf"
    check_library "simdjson"
    check_library "curl"
else
    echo -e "${YELLOW}Skipping library checks on macOS (Homebrew manages deps)${NC}"
fi

#==============================================================================
# Test 9: Compiler Support
#==============================================================================

test_section "Test 9: C++20 Compiler Support"

if command -v g++ &> /dev/null; then
    GCC_VERSION=$(g++ --version | head -n1)
    echo "  Compiler: $GCC_VERSION"
    
    # Test C++20 support
    cat > /tmp/test_cpp20.cpp << 'EOF'
#include <concepts>
#include <ranges>
template<typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;
int main() {
    return 0;
}
EOF
    
    if g++ -std=c++20 /tmp/test_cpp20.cpp -o /tmp/test_cpp20 2>/dev/null; then
        test_passed "C++20 support verified"
        rm -f /tmp/test_cpp20.cpp /tmp/test_cpp20
    else
        test_failed "C++20 support" "Compiler does not support C++20"
        rm -f /tmp/test_cpp20.cpp
    fi
else
    test_failed "Compiler" "g++ not found"
fi

#==============================================================================
# Test 10: Build System
#==============================================================================

test_section "Test 10: Build System"

if [ -f "Makefile.crossplatform" ]; then
    test_passed "Cross-platform Makefile exists"
    
    # Verify OS detection
    DETECTED_OS=$(make -f Makefile.crossplatform help 2>/dev/null | grep "Detected OS:" | cut -d: -f2 | xargs)
    if [ -n "$DETECTED_OS" ]; then
        echo "  Detected OS: $DETECTED_OS"
        test_passed "OS detection working"
    else
        test_failed "OS detection" "Could not detect OS in Makefile"
    fi
else
    test_failed "Build system" "Makefile.crossplatform not found"
fi

#==============================================================================
# Summary
#==============================================================================

echo ""
echo "=================================="
echo "Test Summary"
echo "=================================="
echo ""
echo "Total Tests:  $TOTAL_TESTS"
echo -e "${GREEN}Passed:       $PASSED_TESTS${NC}"
echo -e "${RED}Failed:       $FAILED_TESTS${NC}"
echo ""

if [ $FAILED_TESTS -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED!${NC}"
    echo ""
    echo "System is ready for production use."
    echo ""
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    echo ""
    echo "Please fix the issues above before deploying."
    echo ""
    exit 1
fi
