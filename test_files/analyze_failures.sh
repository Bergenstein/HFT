#!/bin/zsh

echo "=== WHY ARE ALL STRATEGIES LOSING MONEY? ==="
echo ""

DATA="data/raw_20251107_194636_ws0.ndjson"
PRODUCT="LTC-EUR"

echo "1. DATA SAMPLE SIZE:"
echo "   File: $DATA"
LINES=$(wc -l < "$DATA")
echo "   Total lines: $LINES"
./build/scan_recording "$DATA" | grep "LTC-EUR"

echo ""
echo "2. TIME DURATION:"
FIRST_TIME=$(head -2 "$DATA" | tail -1 | grep -o '"time":"[^"]*"' | cut -d'"' -f4 | head -1)
LAST_TIME=$(tail -1 "$DATA" | grep -o '"time":"[^"]*"' | cut -d'"' -f4 | head -1)
echo "   First timestamp: $FIRST_TIME"
echo "   Last timestamp:  $LAST_TIME"
echo "   Duration: ~30 seconds (estimated)"

echo ""
echo "3. REPLAY ORDER BOOK TO SEE PRICE MOVEMENT:"
./build/replay_and_book "$DATA" "$PRODUCT" 2>&1 | head -20

echo ""
echo "4. RUN ONE STRATEGY WITH VERBOSE OUTPUT:"
echo "   Testing: imbalance strategy on $PRODUCT"
./build/backtest_strategy "$DATA" imbalance "$PRODUCT" 0.1 0.6 5 10000 2>&1 | tail -30

echo ""
echo "=== ROOT CAUSE ANALYSIS ==="
echo ""
echo "Problems identified:"
echo "  1. DATA TOO SHORT: Only ~30 seconds of data"
echo "     - Not enough time for prices to move meaningfully"
echo "     - Strategies designed for minutes/hours, not seconds"
echo ""
echo "  2. FEES DOMINATE: On tiny price moves, fees > profit"
echo "     - Need larger price moves to overcome 0.5% fee"
echo "     - Or hold positions longer"
echo ""
echo "  3. NO TREND: 30 seconds shows random noise, not signal"
echo "     - Mean reversion needs actual deviations to revert"
echo "     - Momentum needs actual trends to follow"
echo ""
echo "SOLUTION:"
echo "  Collect HOURS of data, not seconds:"
echo "  ./build/stream_and_record    # Let run for 1+ hours"
echo ""
