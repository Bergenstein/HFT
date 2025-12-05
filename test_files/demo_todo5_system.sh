#!/bin/bash
# Quick Demo of TODO #5 Complete Implementation

echo "=========================================="
echo "  TODO #5 - COMPLETE SYSTEM DEMO"
echo "=========================================="
echo ""
echo "This script demonstrates the complete TODO #5 implementation:"
echo "  1. Multi-exchange data pipeline"
echo "  2. In-memory queue + cache"
echo "  3. SQLite historical storage"
echo "  4. ZeroMQ broadcasting"
echo "  5. Live strategy execution"
echo ""

# Check if binaries exist
if [ ! -f build/complete_multi_exchange_pipeline ]; then
    echo "❌ Error: complete_multi_exchange_pipeline not built"
    echo "Run: make build/complete_multi_exchange_pipeline"
    exit 1
fi

if [ ! -f build/live_strategy_runner ]; then
    echo "❌ Error: live_strategy_runner not built"
    echo "Run: make build/live_strategy_runner"
    exit 1
fi

echo "✅ Both binaries found!"
echo ""
echo "=========================================="
echo "  DEMO INSTRUCTIONS"
echo "=========================================="
echo ""
echo "To run the complete TODO #5 system:"
echo ""
echo "Terminal 1 (Data Pipeline):"
echo "  ./build/complete_multi_exchange_pipeline"
echo ""
echo "Terminal 2 (Strategy Engine):"
echo "  ./build/live_strategy_runner"
echo ""
echo "Terminal 3 (Monitor):"
echo "  watch -n 5 'ls -lh todo5_market_data.db'"
echo ""
echo "The system will:"
echo "  • Connect to Coinbase WebSocket"
echo "  • Normalize and queue market data"
echo "  • Update in-memory cache"
echo "  • Store to SQLite database"
echo "  • Broadcast via ZeroMQ"
echo "  • Run 7 strategies in real-time"
echo "  • Generate trading signals"
echo ""
echo "Press Ctrl+C in each terminal to stop"
echo ""
echo "=========================================="
echo ""

read -p "Start data pipeline now? (y/n) " -n 1 -r
echo ""
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Starting data pipeline..."
    echo "Press Ctrl+C to stop, then run strategy engine in another terminal"
    ./build/complete_multi_exchange_pipeline
fi
