#!/bin/bash
#==============================================================================
# Blackbox Strategy Integration Test
# Tests the complete API-based architecture with system + external strategy
#==============================================================================

set -e

echo "=========================================================================="
echo "  BLACKBOX STRATEGY INTEGRATION TEST"
echo "=========================================================================="
echo ""

# Configuration
RUNTIME=${1:-30}
MIN_SPREAD=${2:-3.0}
HFT_DIR="/Users/israelbergenstein/Desktop/Strategies_System/HFT_Full_Pipeline"
STRAT_DIR="/Users/israelbergenstein/Desktop/Strategies_System/sabi-cppstrategies"
LOG_DIR="/tmp/hft_test_$$"

mkdir -p "$LOG_DIR"

echo "Test Configuration:"
echo "  Runtime:         $RUNTIME seconds"
echo "  Min Spread:      $MIN_SPREAD% APY"
echo "  HFT System:      $HFT_DIR"
echo "  Strategies:      $STRAT_DIR"
echo "  Logs:            $LOG_DIR"
echo ""

# Check binaries exist
echo "Checking binaries..."
if [ ! -f "$HFT_DIR/build/system_api_server" ]; then
    echo "❌ ERROR: system_api_server not found. Run: make api_server"
    exit 1
fi

if [ ! -f "$STRAT_DIR/build/test/example_blackbox_strategy" ]; then
    echo "❌ ERROR: example_blackbox_strategy not found. Run: make blackbox-example"
    exit 1
fi

echo "✓ All binaries present"
echo ""

# Start API server
echo "Starting HFT System API Server..."
cd "$HFT_DIR"
./build/system_api_server "$RUNTIME" > "$LOG_DIR/api_server.log" 2>&1 &
API_PID=$!
echo "  PID: $API_PID"
echo "  Log: $LOG_DIR/api_server.log"

# Wait for server to initialize
echo "  Waiting for server startup..."
sleep 3

# Check if server is running
if ! ps -p $API_PID > /dev/null; then
    echo "❌ ERROR: API server failed to start"
    cat "$LOG_DIR/api_server.log"
    exit 1
fi

echo "✓ API server running"
echo ""

# Start strategy client
echo "Starting Blackbox Strategy Client..."
cd "$STRAT_DIR"
./build/test/example_blackbox_strategy "$RUNTIME" "$MIN_SPREAD" > "$LOG_DIR/strategy.log" 2>&1 &
STRAT_PID=$!
echo "  PID: $STRAT_PID"
echo "  Log: $LOG_DIR/strategy.log"
echo ""

# Monitor both processes
echo "Running integration test..."
echo "  (Press Ctrl+C to stop early)"
echo ""

# Wait for strategy to complete
wait $STRAT_PID
STRAT_EXIT=$?

# Wait for API server to complete
wait $API_PID
API_EXIT=$?

echo ""
echo "=========================================================================="
echo "  TEST RESULTS"
echo "=========================================================================="
echo ""

# Check exit codes
if [ $API_EXIT -eq 0 ]; then
    echo "✓ API Server: SUCCESS (exit code 0)"
else
    echo "❌ API Server: FAILED (exit code $API_EXIT)"
fi

if [ $STRAT_EXIT -eq 0 ]; then
    echo "✓ Strategy Client: SUCCESS (exit code 0)"
else
    echo "❌ Strategy Client: FAILED (exit code $STRAT_EXIT)"
fi

echo ""

# Parse results
echo "API Server Statistics:"
grep "Market Data Published:" "$LOG_DIR/api_server.log" || echo "  (no data)"
grep "Signals Received:" "$LOG_DIR/api_server.log" || echo "  (no data)"

echo ""
echo "Strategy Client Statistics:"
grep "Market Data Received:" "$LOG_DIR/strategy.log" || echo "  (no data)"
grep "Opportunities Found:" "$LOG_DIR/strategy.log" || echo "  (no data)"

echo ""

# Count opportunities
OPP_COUNT=$(grep -c "\[OPPORTUNITY" "$LOG_DIR/strategy.log" || echo "0")
echo "Arbitrage Opportunities Detected: $OPP_COUNT"

echo ""
echo "Detailed logs available at:"
echo "  API Server:  $LOG_DIR/api_server.log"
echo "  Strategy:    $LOG_DIR/strategy.log"
echo ""

# Summary
if [ $API_EXIT -eq 0 ] && [ $STRAT_EXIT -eq 0 ] && [ "$OPP_COUNT" -gt 0 ]; then
    echo "=========================================================================="
    echo "  ✅ INTEGRATION TEST PASSED"
    echo "=========================================================================="
    echo ""
    echo "The blackbox architecture is working correctly:"
    echo "  • System publishes market data via ZeroMQ"
    echo "  • Strategy connects as external binary"
    echo "  • Strategy finds arbitrage opportunities"
    echo "  • System doesn't know strategy logic (blackbox)"
    echo ""
    exit 0
else
    echo "=========================================================================="
    echo "  ❌ INTEGRATION TEST FAILED"
    echo "=========================================================================="
    echo ""
    echo "Check logs for details:"
    echo "  $LOG_DIR/api_server.log"
    echo "  $LOG_DIR/strategy.log"
    echo ""
    exit 1
fi
