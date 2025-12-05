#!/bin/bash
echo "=== COINBASE HFT DATA ANALYSIS ==="
echo ""
echo "1. Data Collection Summary:"
ls -lh data/*.ndjson | awk '{sum+=$5} END {printf "   Total: %.1f MB across %d files\n", sum/1024/1024, NR}'
echo ""

echo "2. Most Active Products (by update frequency):"
for f in data/raw_*.ndjson; do
    ./build/scan_recording "$f" 2>/dev/null | grep -A 15 "Top products"
done | grep -E "USD|USDT|EUR" | head -10
echo ""

echo "3. Recording Duration:"
for f in data/raw_*.ndjson; do
    FIRST=$(head -1 "$f" | grep -o '"timestamp":"[^"]*"' | head -1 | cut -d'"' -f4)
    LAST=$(tail -1 "$f" | grep -o '"timestamp":"[^"]*"' | head -1 | cut -d'"' -f4)
    echo "   $f: $FIRST to $LAST"
done
echo ""

echo "4. Products Captured:"
for f in data/raw_*.ndjson; do
    ./build/scan_recording "$f" 2>/dev/null | grep "products=" | awk '{print $NF}'
done | awk '{sum+=$1} END {print "   Total unique products: ~" sum}'
echo ""

echo "=== NEXT STEPS AS A QUANT ==="
echo "✓ Data captured successfully"
echo "□ Analyze bid-ask spreads"
echo "□ Calculate order book imbalances"
echo "□ Compute microstructure features"
echo "□ Test OFI (Order Flow Imbalance) signals"
echo "□ Backtest strategies"
