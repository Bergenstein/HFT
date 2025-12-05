#!/bin/bash
# Comprehensive multi-strategy, multi-asset backtesting

DATA_FILE="data/raw_20251107_194636_ws0.ndjson"
BACKTEST="./build/backtest_strategy"
CAPITAL=10000

echo "=========================================="
echo "  MULTI-STRATEGY MULTI-ASSET BACKTEST"
echo "=========================================="
echo ""

# Get top 5 products from data file
echo "Scanning data file for products..."
PRODUCTS=$(./build/scan_recording $DATA_FILE 2>/dev/null | grep -E "^\s+[A-Z]" | awk '{print $1}' | head -5)

echo "Testing products:"
echo "$PRODUCTS" | sed 's/^/  /'
echo ""

# Results file
RESULTS_FILE="backtest_results_$(date +%Y%m%d_%H%M%S).csv"
echo "Strategy,Product,Trades,Return%,Sharpe,PnL,Fees,Status" > $RESULTS_FILE

# Test each strategy on each product
test_strategy() {
    STRATEGY=$1
    QTY=$2
    PARAM1=$3
    PARAM2=$4
    
    echo "=========================================="
    echo "Testing Strategy: $STRATEGY"
    echo "Parameters: qty=$QTY, param1=$PARAM1, param2=$PARAM2"
    echo "=========================================="
    
    for PRODUCT in $PRODUCTS; do
        echo ""
        echo "--- $STRATEGY on $PRODUCT ---"
        
        # Run backtest and capture output
        OUTPUT=$($BACKTEST $DATA_FILE $STRATEGY $PRODUCT $QTY $PARAM1 $PARAM2 $CAPITAL 2>&1)
        
        # Parse results
        TRADES=$(echo "$OUTPUT" | grep "^Trades" | awk '{print $3}')
        RETURN=$(echo "$OUTPUT" | grep "^Total Return" | awk '{print $4}' | tr -d '%')
        SHARPE=$(echo "$OUTPUT" | grep "^Sharpe (annual)" | awk '{print $4}')
        PNL=$(echo "$OUTPUT" | grep "^Net PnL" | awk '{print $4}')
        FEES=$(echo "$OUTPUT" | grep "^Fees (total)" | awk '{print $4}')
        
        # Determine status
        if [ ! -z "$SHARPE" ] && [ "$SHARPE" != "N/A" ]; then
            SHARPE_NUM=$(echo "$SHARPE" | tr -d '$,')
            if [ $(echo "$SHARPE_NUM > 1.0" | bc -l 2>/dev/null || echo 0) -eq 1 ]; then
                STATUS="✅"
            else
                STATUS="❌"
            fi
        else
            STATUS="⚠️"
            TRADES="${TRADES:-0}"
            RETURN="${RETURN:-0}"
            SHARPE="N/A"
            PNL="${PNL:-\$0}"
            FEES="${FEES:-\$0}"
        fi
        
        # Print summary
        echo "  Trades: $TRADES | Return: ${RETURN}% | Sharpe: $SHARPE | P&L: $PNL | $STATUS"
        
        # Save to CSV
        echo "$STRATEGY,$PRODUCT,$TRADES,$RETURN,$SHARPE,$PNL,$FEES,$STATUS" >> $RESULTS_FILE
    done
    echo ""
}

# Test all strategies
test_strategy "imbalance" 0.1 0.6 5
test_strategy "ofi" 0.1 0.97 0.15
test_strategy "microprice" 0.1 0.001 5
test_strategy "quote_intensity" 0.1 20 0.1
test_strategy "spread_reversion" 0.1 2.0 10
test_strategy "vpin" 0.1 50 0.3
test_strategy "vw_spread" 0.1 0.001 5

echo ""
echo "=========================================="
echo "  RESULTS SUMMARY"
echo "=========================================="
echo ""
echo "Full results saved to: $RESULTS_FILE"
echo ""

# Show results table
echo "All Results:"
column -t -s',' $RESULTS_FILE

echo ""
echo "Test complete!"
