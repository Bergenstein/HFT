#!/bin/zsh

echo "=== REAL METRICS (NO ANNUALIZATION) ==="
echo ""
echo "Strategy          Product      Trades  Raw Return    Net P&L    Fees"
echo "---------------------------------------------------------------------"

tail -n +2 backtest_results_20251107_212109.csv | while IFS=, read strat prod trades ret sharpe pnl fees stat; do
    if [[ "$trades" == "0" ]]; then
        continue
    fi
    
    pnl_clean=$(echo "$pnl" | tr -d '$')
    fees_clean=$(echo "$fees" | tr -d '$')
    
    printf "%-17s %-12s %-7s %-13s %-10s %-7s\n" \
        "$strat" "$prod" "$trades" "${ret}%" "$pnl_clean" "$fees_clean"
done

echo ""
echo "THE TRUTH:"
echo "  All strategies lose money because:"
echo "  1. Price moved <0.02% in 30 seconds"
echo "  2. Trading fees are 0.5% per round-trip"
echo "  3. You need 25x MORE price movement to break even"
echo ""
echo "WHAT THE DATA SHOWS:"
echo "  LTC-EUR: 87.81 -> 87.83 (0.02% move in 30 sec)"
echo "  Need:    87.81 -> 88.25 (0.5%+ move to profit)"
echo ""
echo "SOLUTION: Collect HOURS of data, not 30 seconds"
echo ""
