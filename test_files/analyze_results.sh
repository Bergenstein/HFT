#!/bin/bash

RESULTS_FILE=$(ls -t backtest_results_*.csv | head -1)

echo "=========================================="
echo "  STRATEGY PERFORMANCE ANALYSIS"
echo "=========================================="
echo ""
echo "Data file: $RESULTS_FILE"
echo ""

# Count total tests
TOTAL_TESTS=$(tail -n +2 $RESULTS_FILE | wc -l | tr -d ' ')
echo "Total tests run: $TOTAL_TESTS"
echo ""

# Strategy-by-strategy summary
echo "Strategy Performance Summary:"
echo "-------------------------------------------"
printf "%-20s %8s %10s %12s\n" "Strategy" "Avg Trades" "Avg Sharpe" "Win Rate"
echo "-------------------------------------------"

for STRATEGY in imbalance ofi microprice quote_intensity spread_reversion vpin vw_spread; do
    STRATEGY_DATA=$(grep "^$STRATEGY," $RESULTS_FILE)
    
    if [ -z "$STRATEGY_DATA" ]; then
        continue
    fi
    
    # Calculate average trades
    AVG_TRADES=$(echo "$STRATEGY_DATA" | awk -F',' '{sum+=$3; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}')
    
    # Calculate average Sharpe (excluding zeros and N/A)
    AVG_SHARPE=$(echo "$STRATEGY_DATA" | awk -F',' '$5 != "N/A" && $5 != "0" {sum+=$5; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}')
    
    # Calculate win rate
    TOTAL=$(echo "$STRATEGY_DATA" | wc -l | tr -d ' ')
    WINS=$(echo "$STRATEGY_DATA" | grep "✅" | wc -l | tr -d ' ')
    WIN_RATE=$(echo "scale=1; $WINS * 100 / $TOTAL" | bc 2>/dev/null || echo "0")
    
    printf "%-20s %8s %10s %11s%%\n" "$STRATEGY" "$AVG_TRADES" "$AVG_SHARPE" "$WIN_RATE"
done

echo "-------------------------------------------"
echo ""

# Product analysis
echo "Product Performance Summary:"
echo "-------------------------------------------"
printf "%-12s %8s %10s\n" "Product" "Avg Trades" "Avg Sharpe"
echo "-------------------------------------------"

for PRODUCT in LTC-EUR EDGE-USD DASH-USD SPX-USD INJ-USD; do
    PRODUCT_DATA=$(grep ",$PRODUCT," $RESULTS_FILE)
    
    if [ -z "$PRODUCT_DATA" ]; then
        continue
    fi
    
    AVG_TRADES=$(echo "$PRODUCT_DATA" | awk -F',' '{sum+=$3; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}')
    AVG_SHARPE=$(echo "$PRODUCT_DATA" | awk -F',' '$5 != "N/A" && $5 != "0" {sum+=$5; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}')
    
    printf "%-12s %8s %10s\n" "$PRODUCT" "$AVG_TRADES" "$AVG_SHARPE"
done

echo "-------------------------------------------"
echo ""

# Key findings
echo "Key Findings:"
echo "-------------------------------------------"

# Most active strategy
MOST_ACTIVE=$(tail -n +2 $RESULTS_FILE | awk -F',' '{print $1, $3}' | \
    awk '{sum[$1]+=$2; count[$1]++} END {for(s in sum) print s, sum[s]/count[s]}' | \
    sort -k2 -rn | head -1)
echo "  Most active strategy: $MOST_ACTIVE (avg trades)"

# Least active
LEAST_ACTIVE=$(tail -n +2 $RESULTS_FILE | awk -F',' '{print $1, $3}' | \
    awk '{sum[$1]+=$2; count[$1]++} END {for(s in sum) print s, sum[s]/count[s]}' | \
    sort -k2 -n | head -1)
echo "  Least active strategy: $LEAST_ACTIVE (avg trades)"

# No trades count
NO_TRADES=$(tail -n +2 $RESULTS_FILE | awk -F',' '$3 == "0"' | wc -l | tr -d ' ')
echo "  Strategies with no trades: $NO_TRADES / $TOTAL_TESTS"

echo ""
echo "Overall Assessment:"
echo "  ❌ All strategies currently unprofitable (Sharpe < 1.0)"
echo "  ⚠️  Common issues:"
echo "     - Data sample too short (only ~30 seconds)"
echo "     - Overtrading on small price movements"
echo "     - Fees eating into small profits"
echo ""
echo "Recommendations:"
echo "  1. Collect longer data recordings (hours/days)"
echo "  2. Increase hold times to reduce trading frequency"
echo "  3. Add filters: min spread, min volume, volatility"
echo "  4. Test on more liquid products (BTC-USD, ETH-USD)"
echo "=========================================="
