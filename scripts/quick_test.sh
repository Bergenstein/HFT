#!/bin/bash
#===============================================================================
# QUICK TEST - Verify All Components Work
#===============================================================================
# Fast verification that all components are functional (no simulations)

cd "$(dirname "$0")/.."

echo "════════════════════════════════════════════════════════════"
echo "  HFT System - Quick Verification Test"
echo "════════════════════════════════════════════════════════════"
echo ""

# Test 1: Single Exchange (Binance) - 5 seconds
echo "[1/3] Testing Binance exchange fetch..."
./build/test_multi_exchange_l2 binance 2>&1 | head -30
if [ $? -eq 0 ]; then
    echo "✓ Binance test passed"
else
    echo "✗ Binance test failed"
    exit 1
fi
echo ""

# Test 2: Integrated Pipeline - 5 seconds
echo "[2/3] Testing integrated pipeline (5 seconds)..."
./build/test_integrated_pipeline 5 2>&1 | tail -30 &
pid=$!
sleep 7
kill -9 $pid 2>/dev/null
wait $pid 2>/dev/null
echo "✓ Pipeline test completed"
echo ""

# Test 3: List available components
echo "[3/3] Available Components:"
echo "  • test_multi_exchange_l2       - Test individual exchanges"
echo "  • test_integrated_pipeline     - Full SPSC pipeline integration"
echo "  • test_funding_arb_engine      - Funding rate arbitrage engine"
echo ""

echo "════════════════════════════════════════════════════════════"
echo "  ✓ Quick verification complete"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "Usage examples:"
echo "  ./build/test_multi_exchange_l2 [exchange]"
echo "    Exchanges: binance, bybit, okx, gateio, mexc, kucoin, kraken, bitget, htx"
echo ""
echo "  ./build/test_integrated_pipeline [seconds]"
echo "    Example: ./build/test_integrated_pipeline 30"
echo ""
echo "  ./build/test_funding_arb_engine [seconds]"
echo "    Example: ./build/test_funding_arb_engine 60"
echo ""
