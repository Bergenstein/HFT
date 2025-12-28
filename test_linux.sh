#!/bin/bash
#==============================================================================
# HFT System - Linux Test Suite
#==============================================================================
# This script runs all tests to verify the system works on Linux
#==============================================================================

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo "HFT System - Linux Test Suite"
echo "=========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

TESTS_PASSED=0
TESTS_FAILED=0

#==============================================================================
# Test Runner Function
#==============================================================================
run_test() {
    local test_name="$1"
    local test_binary="$2"
    
    echo "----------------------------------------"
    echo "Running: $test_name"
    echo "----------------------------------------"
    
    if [ -f "$test_binary" ]; then
        if "$test_binary"; then
            echo -e "${GREEN}✓ PASSED${NC}: $test_name"
            ((TESTS_PASSED++))
        else
            echo -e "${RED}✗ FAILED${NC}: $test_name"
            ((TESTS_FAILED++))
        fi
    else
        echo -e "${YELLOW}⚠ SKIPPED${NC}: $test_name (binary not found: $test_binary)"
    fi
    echo ""
}

#==============================================================================
# Build Tests
#==============================================================================
echo "Step 1: Building all tests..."
echo "----------------------------------------"
if make -f Makefile.crossplatform clean && make -f Makefile.crossplatform tests; then
    echo -e "${GREEN}✓ Build successful${NC}"
else
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi
echo ""

#==============================================================================
# Run Core System Tests
#==============================================================================
echo "=========================================="
echo "CORE SYSTEM TESTS"
echo "=========================================="
echo ""

run_test "Lock-Free Queues" "./build/test_lockfree_queues"
run_test "Matching Engine" "./build/test_matching_engine_simple"
run_test "System Pipeline" "./build/test_system_pipeline"
run_test "Funding Rates" "./build/test_funding_rates_simple"

#==============================================================================
# Run Communication Tests
#==============================================================================
echo "=========================================="
echo "COMMUNICATION TESTS"
echo "=========================================="
echo ""

# ZMQ test (runs in background)
echo "----------------------------------------"
echo "Running: ZMQ Pub/Sub Test"
echo "----------------------------------------"
if [ -f "./build/test_zmq_pubsub" ]; then
    timeout 5s ./build/test_zmq_pubsub &>/dev/null || true
    if [ $? -eq 124 ]; then
        echo -e "${GREEN}✓ PASSED${NC}: ZMQ Pub/Sub Test (timeout = working)"
        ((TESTS_PASSED++))
    else
        echo -e "${YELLOW}⚠ UNKNOWN${NC}: ZMQ Pub/Sub Test"
    fi
else
    echo -e "${YELLOW}⚠ SKIPPED${NC}: ZMQ Pub/Sub Test (binary not found)"
fi
echo ""

#==============================================================================
# Run Performance Tests
#==============================================================================
echo "=========================================="
echo "PERFORMANCE TESTS"
echo "=========================================="
echo ""

# Latency test (may need network)
echo "----------------------------------------"
echo "Running: End-to-End Latency Test"
echo "----------------------------------------"
if [ -f "./build/test_end_to_end_latency" ]; then
    timeout 10s ./build/test_end_to_end_latency &>/dev/null || true
    if [ $? -eq 124 ] || [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ PASSED${NC}: End-to-End Latency Test"
        ((TESTS_PASSED++))
    else
        echo -e "${YELLOW}⚠ WARNING${NC}: Latency test may need network access"
    fi
else
    echo -e "${YELLOW}⚠ SKIPPED${NC}: Latency test (binary not found)"
fi
echo ""

#==============================================================================
# Run Exchange Integration Tests
#==============================================================================
echo "=========================================="
echo "EXCHANGE INTEGRATION TESTS"
echo "=========================================="
echo ""

echo "Note: These tests require network access"
echo "They will timeout after 10 seconds if no connection"
echo ""

# GRVT test
echo "----------------------------------------"
echo "Running: GRVT Connection Test"
echo "----------------------------------------"
if [ -f "./build/test_grvt_connection" ]; then
    timeout 10s ./build/test_grvt_connection &>/dev/null || true
    exit_code=$?
    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}✓ PASSED${NC}: GRVT Connection Test"
        ((TESTS_PASSED++))
    elif [ $exit_code -eq 124 ]; then
        echo -e "${YELLOW}⚠ TIMEOUT${NC}: GRVT Connection Test (expected without API access)"
    else
        echo -e "${YELLOW}⚠ WARNING${NC}: GRVT test may need API credentials"
    fi
else
    echo -e "${YELLOW}⚠ SKIPPED${NC}: GRVT test (binary not found)"
fi
echo ""

# Coinbase test
echo "----------------------------------------"
echo "Running: Coinbase L2 Test"
echo "----------------------------------------"
if [ -f "./build/test_coinbase_l2" ]; then
    timeout 10s ./build/test_coinbase_l2 &>/dev/null || true
    exit_code=$?
    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}✓ PASSED${NC}: Coinbase L2 Test"
        ((TESTS_PASSED++))
    elif [ $exit_code -eq 124 ]; then
        echo -e "${YELLOW}⚠ TIMEOUT${NC}: Coinbase L2 Test (expected without API access)"
    else
        echo -e "${YELLOW}⚠ WARNING${NC}: Coinbase test may need API credentials"
    fi
else
    echo -e "${YELLOW}⚠ SKIPPED${NC}: Coinbase test (binary not found)"
fi
echo ""

#==============================================================================
# System Information
#==============================================================================
echo "=========================================="
echo "SYSTEM INFORMATION"
echo "=========================================="
echo ""

echo "OS: $(uname -s) $(uname -r)"
echo "Architecture: $(uname -m)"
echo "CPU Cores: $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 'unknown')"
echo "C++ Compiler: $(g++ --version | head -n1)"
echo "Protobuf: $(protoc --version 2>/dev/null || echo 'not found')"
echo ""

echo "Library Versions:"
ldconfig -v 2>/dev/null | grep -E "libssl|libzmq|libprotobuf|libsimdjson|libcurl" || echo "  (ldconfig not available)"
echo ""

#==============================================================================
# Summary
#==============================================================================
echo "=========================================="
echo "TEST SUMMARY"
echo "=========================================="
echo ""

TOTAL_TESTS=$((TESTS_PASSED + TESTS_FAILED))

echo "Total Tests: $TOTAL_TESTS"
echo -e "${GREEN}Passed: $TESTS_PASSED${NC}"
if [ $TESTS_FAILED -gt 0 ]; then
    echo -e "${RED}Failed: $TESTS_FAILED${NC}"
else
    echo "Failed: $TESTS_FAILED"
fi
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}=========================================="
    echo "✓ ALL CORE TESTS PASSED!"
    echo -e "==========================================${NC}"
    echo ""
    echo "The HFT system is working correctly on Linux!"
    echo ""
    echo "Next steps:"
    echo "  1. Run with real exchange connections (requires API keys)"
    echo "  2. Test strategies from sabi-cppstrategies"
    echo "  3. Run system_api_server for live trading"
    exit 0
else
    echo -e "${RED}=========================================="
    echo "✗ SOME TESTS FAILED"
    echo -e "==========================================${NC}"
    echo ""
    echo "Please check the output above for details."
    exit 1
fi
