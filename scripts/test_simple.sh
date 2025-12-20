#!/bin/bash
# Simple test runner without timeout (for macOS compatibility)
cd "$(dirname "$0")/.."

if [[ ! -f "build/test_multi_exchange_l2" ]]; then
    echo "Building test binary..."
    ./scripts/build_tests.sh
fi

if [[ "$1" == "pipeline" ]]; then
    echo "Running integrated pipeline test (5 seconds)..."
    ./build/test_integrated_pipeline 5
elif [[ "$1" == "all" ]]; then
    echo "Testing all exchanges sequentially..."
    for exchange in binance bybit okx gateio mexc kucoin kraken bitget htx; do
        echo "━━━ Testing $exchange ━━━"
        ./build/test_multi_exchange_l2 "$exchange"
        echo ""
    done
elif [[ -n "$1" ]]; then
    echo "Testing $1..."
    ./build/test_multi_exchange_l2 "$1"
else
    echo "Usage: $0 [exchange|pipeline|all]"
    echo ""
    echo "Available exchanges:"
    echo "  binance, bybit, okx, gateio, mexc, kucoin, kraken, bitget, htx, bingx"
    echo ""
    echo "Special commands:"
    echo "  pipeline - Run integrated pipeline test"
    echo "  all      - Run all tests sequentially"
fi
