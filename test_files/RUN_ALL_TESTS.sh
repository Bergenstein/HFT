#!/bin/bash
# Master test script - runs ALL system tests

echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║        COMPLETE HFT SYSTEM TEST SUITE                         ║"
echo "║        Original System + Multi-Exchange Arbitrage             ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

TOTAL_PASS=0
TOTAL_FAIL=0

# Part 1: Build Tests
echo -e "${BLUE}═══ PART 1: BUILD SYSTEM ═══${NC}"
echo ""

echo "Building core HFT system..."
if make stream replay backtest scan > /dev/null 2>&1; then
    echo -e "${GREEN}✓${NC} Core HFT system built"
    ((TOTAL_PASS++))
else
    echo -e "${RED}✗${NC} Core build failed"
    ((TOTAL_FAIL++))
fi

echo "Building arbitrage system..."
if make arb_demo > /dev/null 2>&1; then
    echo -e "${GREEN}✓${NC} Arbitrage system built"
    ((TOTAL_PASS++))
else
    echo -e "${RED}✗${NC} Arbitrage build failed"
    ((TOTAL_FAIL++))
fi

# Part 2: Data Tests
echo ""
echo -e "${BLUE}═══ PART 2: DATA PROCESSING ═══${NC}"
echo ""

LATEST_DATA=$(ls -t data/*.ndjson 2>/dev/null | head -n1)
if [ -n "$LATEST_DATA" ]; then
    echo "Using data file: $LATEST_DATA"
    
    echo "Testing data scanner..."
    if ./build/scan_recording "$LATEST_DATA" 2>&1 | grep -q "product_id"; then
        echo -e "${GREEN}✓${NC} Data scanner working"
        ((TOTAL_PASS++))
    else
        echo -e "${RED}✗${NC} Data scanner failed"
        ((TOTAL_FAIL++))
    fi
    
    echo "Testing order book replay..."
    if timeout 5 ./build/replay_and_book "$LATEST_DATA" 2>&1 | grep -q "bb=\|ba="; then
        echo -e "${GREEN}✓${NC} Order book replay working"
        ((TOTAL_PASS++))
    else
        echo -e "${RED}✗${NC} Order book replay failed"
        ((TOTAL_FAIL++))
    fi
else
    echo -e "${RED}✗${NC} No data files found (run ./build/stream_and_record first)"
    ((TOTAL_FAIL+=2))
fi

# Part 3: Arbitrage Tests
echo ""
echo -e "${BLUE}═══ PART 3: ARBITRAGE SYSTEM ═══${NC}"
echo ""

echo "Running arbitrage demo..."
ARB_OUTPUT=$(./build/test_arbitrage_demo 2>&1)

if echo "$ARB_OUTPUT" | grep -q "OPPORTUNITIES FOR"; then
    echo -e "${GREEN}✓${NC} Arbitrage detection working"
    ((TOTAL_PASS++))
else
    echo -e "${RED}✗${NC} Arbitrage detection failed"
    ((TOTAL_FAIL++))
fi

if echo "$ARB_OUTPUT" | grep -q "435.*bps"; then
    echo -e "${GREEN}✓${NC} Large opportunity detection"
    ((TOTAL_PASS++))
else
    echo -e "${RED}✗${NC} Large opportunity missed"
    ((TOTAL_FAIL++))
fi

if echo "$ARB_OUTPUT" | grep -q "No profitable opportunities"; then
    echo -e "${GREEN}✓${NC} Filtering unprofitable trades"
    ((TOTAL_PASS++))
else
    echo -e "${RED}✗${NC} Filter not working"
    ((TOTAL_FAIL++))
fi

# Part 4: Performance Tests
echo ""
echo -e "${BLUE}═══ PART 4: PERFORMANCE ═══${NC}"
echo ""

echo "Testing arbitrage engine speed..."
START=$(date +%s%N)
./build/test_arbitrage_demo > /dev/null 2>&1
END=$(date +%s%N)
DURATION=$(( (END - START) / 1000000 ))  # Convert to ms

if [ $DURATION -lt 1000 ]; then
    echo -e "${GREEN}✓${NC} Engine fast enough (${DURATION}ms < 1000ms)"
    ((TOTAL_PASS++))
else
    echo -e "${RED}✗${NC} Engine too slow (${DURATION}ms)"
    ((TOTAL_FAIL++))
fi

# Part 5: Integration Tests
echo ""
echo -e "${BLUE}═══ PART 5: INTEGRATION ═══${NC}"
echo ""

echo "Checking file structure..."
REQUIRED_DIRS="arb exchanges strats core run build data"
MISSING=0
for dir in $REQUIRED_DIRS; do
    if [ ! -d "$dir" ]; then
        echo -e "${RED}✗${NC} Missing directory: $dir"
        ((MISSING++))
    fi
done

if [ $MISSING -eq 0 ]; then
    echo -e "${GREEN}✓${NC} All required directories present"
    ((TOTAL_PASS++))
else
    echo -e "${RED}✗${NC} Missing $MISSING directories"
    ((TOTAL_FAIL++))
fi

echo "Checking documentation..."
DOCS="COMPLETE_SYSTEM_STATUS.md ARBITRAGE_SYSTEM_STATUS.md arb/README.md"
DOC_MISSING=0
for doc in $DOCS; do
    if [ ! -f "$doc" ]; then
        ((DOC_MISSING++))
    fi
done

if [ $DOC_MISSING -eq 0 ]; then
    echo -e "${GREEN}✓${NC} Documentation complete"
    ((TOTAL_PASS++))
else
    echo -e "${RED}✗${NC} Missing documentation"
    ((TOTAL_FAIL++))
fi

# Final Summary
echo ""
echo "══════════════════════════════════════════════════════════════"
echo "                    FINAL RESULTS"
echo "══════════════════════════════════════════════════════════════"
echo -e "Tests Passed: ${GREEN}$TOTAL_PASS${NC}"
echo -e "Tests Failed: ${RED}$TOTAL_FAIL${NC}"
TOTAL=$((TOTAL_PASS + TOTAL_FAIL))
if [ $TOTAL -gt 0 ]; then
    PERCENT=$((TOTAL_PASS * 100 / TOTAL))
    echo "Success Rate: $PERCENT%"
fi
echo ""

if [ $TOTAL_FAIL -eq 0 ]; then
    echo -e "${GREEN}╔═══════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║  ✓ ALL SYSTEMS OPERATIONAL                        ║${NC}"
    echo -e "${GREEN}║  Ready for integration and deployment             ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════════════╝${NC}"
    exit 0
else
    echo -e "${RED}╔═══════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║  ✗ SOME TESTS FAILED                              ║${NC}"
    echo -e "${RED}║  Review failures before proceeding                ║${NC}"
    echo -e "${RED}╚═══════════════════════════════════════════════════╝${NC}"
    exit 1
fi
