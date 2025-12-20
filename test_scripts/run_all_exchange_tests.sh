#!/bin/bash
#===============================================================================
# MULTI-EXCHANGE TEST RUNNER
#===============================================================================
# Tests each exchange individually and the integrated pipeline
# Usage: ./run_all_exchange_tests.sh [quick|full]

set -e  # Exit on error

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
MODE="${1:-quick}"  # quick or full
QUICK_RUNTIME=10    # seconds
FULL_RUNTIME=60     # seconds

echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║          MULTI-EXCHANGE L2 + FUNDING TEST SUITE                 ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
echo ""

# Ensure we're in the right directory
cd "$(dirname "$0")/.."

# Build tests
echo -e "${BLUE}[BUILD]${NC} Building test binaries..."
make test_multi_exchange_l2 > /dev/null 2>&1
make test_integrated_pipeline > /dev/null 2>&1
echo -e "${GREEN}✓${NC} Tests built successfully"
echo ""

#===============================================================================
# PHASE 1: Individual Exchange Tests
#===============================================================================
echo "════════════════════════════════════════════════════════════════════"
echo " PHASE 1: Individual Exchange Tests"
echo "════════════════════════════════════════════════════════════════════"
echo ""

EXCHANGES=("binance" "bybit" "okx" "gateio" "mexc" "kucoin" "kraken" "bitget" "htx")
PASSED=0
FAILED=0

for exchange in "${EXCHANGES[@]}"; do
    echo -e "${BLUE}Testing ${exchange}...${NC}"
    
    if OUTPUT=$(make test_${exchange} 2>&1); then
        # Check if we got market data
        MARKETS=$(echo "$OUTPUT" | grep -c "Symbol:" || echo "0")
        
        if [ "$MARKETS" -gt 0 ]; then
            echo -e "${GREEN}✓ ${exchange}: ${MARKETS} markets fetched${NC}"
            ((PASSED++))
        else
            echo -e "${RED}✗ ${exchange}: No markets fetched${NC}"
            ((FAILED++))
        fi
    else
        echo -e "${RED}✗ ${exchange}: Test failed${NC}"
        echo "$OUTPUT" | tail -10
        ((FAILED++))
    fi
    echo ""
done

echo "════════════════════════════════════════════════════════════════════"
echo " Individual Exchange Tests: ${PASSED}/${#EXCHANGES[@]} passed"
echo "════════════════════════════════════════════════════════════════════"
echo ""

#===============================================================================
# PHASE 2: Integrated Pipeline Test
#===============================================================================
echo "════════════════════════════════════════════════════════════════════"
echo " PHASE 2: Integrated Pipeline Test"
echo "════════════════════════════════════════════════════════════════════"
echo ""

if [ "$MODE" = "full" ]; then
    RUNTIME=$FULL_RUNTIME
    echo -e "${YELLOW}Running FULL test (${RUNTIME} seconds)...${NC}"
else
    RUNTIME=$QUICK_RUNTIME
    echo -e "${YELLOW}Running QUICK test (${RUNTIME} seconds)...${NC}"
fi
echo ""

# Run integrated pipeline test
DB_FILE="test_pipeline_$(date +%Y%m%d_%H%M%S).db"

echo "Starting integrated pipeline..."
if OUTPUT=$(./build/test_integrated_pipeline $RUNTIME true "$DB_FILE" 2>&1); then
    # Parse results
    UPDATES=$(echo "$OUTPUT" | grep "Total updates:" | awk '{print $3}')
    SYMBOLS=$(echo "$OUTPUT" | grep "Symbols tracked:" | awk '{print $3}')
    OPPORTUNITIES=$(echo "$OUTPUT" | grep "^  [0-9]\\." | wc -l | tr -d ' ')
    
    echo ""
    echo "════════════════════════════════════════════════════════════════════"
    echo " Integrated Pipeline Results:"
    echo "════════════════════════════════════════════════════════════════════"
    echo "  Market Data Updates: ${UPDATES:-0}"
    echo "  Unique Symbols: ${SYMBOLS:-0}"
    echo "  Arbitrage Opportunities: ${OPPORTUNITIES:-0}"
    echo "  Database: ${DB_FILE}"
    echo "════════════════════════════════════════════════════════════════════"
    echo ""
    
    if [ "${UPDATES:-0}" -gt 0 ]; then
        echo -e "${GREEN}✓ Integrated pipeline test PASSED${NC}"
        
        # Show top opportunities
        echo ""
        echo "Top Arbitrage Opportunities (>20% APY):"
        echo "$OUTPUT" | grep "^  [0-9]\\." | head -5
        echo ""
    else
        echo -e "${YELLOW}⚠ Integrated pipeline ran but no data processed${NC}"
    fi
else
    echo -e "${RED}✗ Integrated pipeline test FAILED${NC}"
    echo "$OUTPUT" | tail -20
    ((FAILED++))
fi

#===============================================================================
# Summary
#===============================================================================
echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║                         TEST SUMMARY                             ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
echo ""
echo "  Individual Exchanges: ${PASSED}/${#EXCHANGES[@]} passed"
echo "  Integrated Pipeline: $([ "${UPDATES:-0}" -gt 0 ] && echo "✓ PASSED" || echo "✗ FAILED")"
echo ""
echo "  Test Duration: ${RUNTIME} seconds"
echo "  Database Saved: ${DB_FILE}"
echo ""

if [ $FAILED -eq 0 ] && [ "${UPDATES:-0}" -gt 0 ]; then
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║                    ALL TESTS PASSED! ✓                          ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════════════╝${NC}"
    exit 0
else
    echo -e "${RED}╔══════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║                   SOME TESTS FAILED! ✗                          ║${NC}"
    echo -e "${RED}╚══════════════════════════════════════════════════════════════════╝${NC}"
    exit 1
fi
