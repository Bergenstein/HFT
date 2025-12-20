#!/bin/bash
# Quick test script for individual exchanges
# Usage: ./scripts/test.sh [exchange_name]
#   e.g.: ./scripts/test.sh binance
#         ./scripts/test.sh all

cd "$(dirname "$0")/.."

# Timeout function for macOS
run_with_timeout() {
    local timeout=$1
    shift
    ( "$@" ) & pid=$!
    ( sleep "$timeout" && kill -HUP $pid 2>/dev/null ) &
    wait $pid 2>/dev/null
}

if [[ ! -f "build/test_multi_exchange_l2" ]]; then
    echo "Building test binary..."
    ./scripts/build_tests.sh
fi

if [[ "$1" == "pipeline" ]]; then
    echo "Running integrated pipeline test (10 seconds)..."
    run_with_timeout 15 ./build/test_integrated_pipeline 10
elif [[ "$1" == "funding" ]]; then
    echo "Running funding rate arbitrage engine test (20 seconds)..."
    run_with_timeout 25 ./build/test_funding_arb_engine 20
elif [[ "$1" == "all" ]]; then
    ./scripts/run_all_tests.sh
elif [[ -n "$1" ]]; then
    echo "Testing $1..."
    run_with_timeout 10 ./build/test_multi_exchange_l2 "$1"
else
    echo "Usage: $0 [exchange|pipeline|funding|all]"
    echo ""
    echo "Available exchanges:"
    echo "  binance, bybit, okx, gateio, mexc, kucoin, kraken, bitget, htx, bingx"
    echo ""
    echo "Special commands:"
    echo "  pipeline - Run integrated pipeline test"
    echo "  funding  - Run funding rate arbitrage engine test"
    echo "  all      - Run all tests"
fi
