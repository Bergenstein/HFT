#!/bin/bash
# Run all HFT system tests
# Usage: ./run_all_tests.sh

set -e  # Exit on error

echo "=========================================="
echo "  HFT SYSTEM - AUTOMATED TEST SUITE"
echo "=========================================="
echo ""

# Test 1: Build integration test
echo "[1/4] Building integration test..."
make test_integration > /dev/null 2>&1
echo "✓ Build successful"
echo ""

# Test 2: Run integration test
echo "[2/4] Running hot/cold path integration test (10 seconds)..."
rm -f test_integration.db
./build/test_hot_cold_integration &
TEST_PID=$!
sleep 10
kill -INT $TEST_PID 2>/dev/null || true
wait $TEST_PID 2>/dev/null || true
echo ""

# Test 3: Verify database
echo "[3/4] Verifying database..."
QUOTE_COUNT=$(sqlite3 test_integration.db "SELECT COUNT(*) FROM quotes;" 2>/dev/null || echo "0")
if [ "$QUOTE_COUNT" -gt 500 ]; then
    echo "✓ Database verified: $QUOTE_COUNT quotes archived"
else
    echo "✗ Database check failed: only $QUOTE_COUNT quotes (expected >500)"
    exit 1
fi
echo ""

# Test 4: Arbitrage strategies
echo "[4/4] Testing arbitrage strategies..."
cd test_files
g++ -std=c++20 -I.. test_new_arb_strategies.cpp -o ../build/test_new_arb_strategies 2>/dev/null
../build/test_new_arb_strategies > /tmp/arb_test_output.txt 2>&1
if grep -qi "ALL TESTS PASSED" /tmp/arb_test_output.txt; then
    echo "✓ All arbitrage strategies passed"
else
    echo "✗ Arbitrage tests failed"
    cat /tmp/arb_test_output.txt
    exit 1
fi
cd ..
echo ""

echo "=========================================="
echo "  ALL TESTS PASSED ✅"
echo "=========================================="
echo ""
echo "Summary:"
echo "  - Integration test: ✅ PASS ($QUOTE_COUNT quotes)"
echo "  - Hot path processor: ✅ PASS"
echo "  - Cold path aggregator: ✅ PASS"
echo "  - Arbitrage strategies: ✅ PASS"
echo ""
echo "Next steps:"
echo "  - See MASTER_DOCUMENTATION.md for full details"
echo "  - See NOTES/TESTING_GUIDE.md for advanced testing"
echo ""
