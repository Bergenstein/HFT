#!/bin/bash
#===============================================================================
# RUN ALL EXCHANGE TESTS - Comprehensive Test Suite
#===============================================================================
# Tests all 10 exchanges individually + integrated pipeline
# Usage: ./scripts/run_all_tests.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "════════════════════════════════════════════════════════════"
echo "  HFT Multi-Exchange Test Suite"
echo "════════════════════════════════════════════════════════════"
echo ""

# Check if binaries exist
if [[ ! -f "build/test_multi_exchange_l2" ]]; then
    echo -e "${RED}✗ Test binary not found. Building...${NC}"
    ./scripts/build_tests.sh
    echo ""
fi

PASS=0
FAIL=0
TOTAL=0

# Timeout function for macOS
run_with_timeout() {
    local timeout=$1
    shift
    ( "$@" ) & pid=$!
    ( sleep "$timeout" && kill -HUP $pid 2>/dev/null ) &
    wait $pid 2>/dev/null
}

# Function to test an exchange
test_exchange() {
    local exchange=$1
    local name=$2
    TOTAL=$((TOTAL + 1))
    
    echo -e "${YELLOW}━━━ Testing $name ━━━${NC}"
    if run_with_timeout 10 ./build/test_multi_exchange_l2 "$exchange" 2>&1 | grep -q "markets retrieved"; then
        echo -e "${GREEN}✓ $name PASSED${NC}"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}✗ $name FAILED${NC}"
        FAIL=$((FAIL + 1))
    fi
    echo ""
}

# Test all exchanges
test_exchange "binance" "Binance Futures"
test_exchange "bybit" "Bybit Perpetuals"
test_exchange "okx" "OKX Perpetuals"
test_exchange "gateio" "Gate.io Futures"
test_exchange "mexc" "MEXC Futures"
test_exchange "kucoin" "KuCoin Futures"
test_exchange "kraken" "Kraken Futures"
test_exchange "bitget" "Bitget Futures"
test_exchange "htx" "HTX Futures"
test_exchange "bingx" "BingX Futures"

# Test integrated pipeline (short 5-second test)
echo -e "${YELLOW}━━━ Testing Integrated Pipeline ━━━${NC}"
TOTAL=$((TOTAL + 1))
if run_with_timeout 15 ./build/test_integrated_pipeline 5 2>&1 | grep -q "opportunities found"; then
    echo -e "${GREEN}✓ Integrated Pipeline PASSED${NC}"
    PASS=$((PASS + 1))
else
    echo -e "${RED}✗ Integrated Pipeline FAILED${NC}"
    FAIL=$((FAIL + 1))
fi
echo ""

# Summary
echo "════════════════════════════════════════════════════════════"
echo "  Test Results"
echo "════════════════════════════════════════════════════════════"
echo -e "  Total Tests:  $TOTAL"
echo -e "  ${GREEN}Passed:       $PASS${NC}"
if [[ $FAIL -gt 0 ]]; then
    echo -e "  ${RED}Failed:       $FAIL${NC}"
else
    echo -e "  Failed:       $FAIL"
fi
echo "════════════════════════════════════════════════════════════"

if [[ $FAIL -eq 0 ]]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED${NC}"
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    exit 1
fi
