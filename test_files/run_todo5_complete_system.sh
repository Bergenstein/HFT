#!/bin/bash
# run_todo5_complete_system.sh - Complete TODO Item #5 Demo

echo "========================================"
echo "  TODO ITEM #5 - COMPLETE SYSTEM"
echo "========================================"
echo ""
echo "This script demonstrates the complete implementation of TODO #5:"
echo "1. Multi-exchange data retrieval"
echo "2. Data normalization"
echo "3. Lock-free in-memory queues"
echo "4. In-memory cache (latest quotes)"
echo "5. SQLite historical storage"
echo "6. ZeroMQ broadcasting to strategies"
echo ""
echo "========================================"
echo ""

# Check if binaries are built
if [ ! -f "build/complete_multi_exchange_pipeline" ]; then
    echo "[BUILD] Building complete_multi_exchange_pipeline..."
    make multi_exchange
    if [ $? -ne 0 ]; then
        echo "❌ Build failed! Please fix compilation errors."
        exit 1
    fi
fi

if [ ! -f "build/live_strategy_runner" ]; then
    echo "[BUILD] Building live_strategy_runner..."
    make live_strategy
    if [ $? -ne 0 ]; then
        echo "❌ Build failed! Please fix compilation errors."
        exit 1
    fi
fi

echo "✓ All binaries ready"
echo ""

# Create tmux session or run in sequence
if command -v tmux &> /dev/null; then
    echo "[TMUX] Starting in split-pane mode..."
    echo "  - Left pane:  Data pipeline (publisher)"
    echo "  - Right pane: Strategy engine (subscriber)"
    echo ""
    echo "Press Ctrl+C in either pane to stop"
    echo "Press Ctrl+B then D to detach from tmux"
    echo ""
    
    # Create new tmux session
    tmux new-session -d -s todo5 "build/complete_multi_exchange_pipeline"
    tmux split-window -h "sleep 3 && build/live_strategy_runner"
    tmux attach-session -t todo5
else
    echo "[WARNING] tmux not found - running in sequence mode"
    echo ""
    echo "Starting data pipeline (will run for 30 seconds)..."
    echo "Then starting strategy engine..."
    echo ""
    
    # Run pipeline for 30 seconds
    timeout 30 build/complete_multi_exchange_pipeline &
    PIPELINE_PID=$!
    
    # Wait for pipeline to start
    sleep 3
    
    # Run strategy engine for 25 seconds
    timeout 25 build/live_strategy_runner
    
    # Wait for pipeline to finish
    wait $PIPELINE_PID
fi

echo ""
echo "========================================"
echo "  TODO ITEM #5 - DEMO COMPLETE"
echo "========================================"
echo ""
echo "Check the following:"
echo "  1. multi_exchange_data.db - SQLite database with quotes"
echo "  2. Terminal output - Trading signals generated"
echo "  3. Pipeline statistics - Throughput metrics"
echo ""
