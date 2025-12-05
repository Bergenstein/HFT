#!/bin/bash

echo "=========================================="
echo "HFT PLATFORM - COMPLETE SYSTEM DEMO"
echo "=========================================="
echo ""

# Check we're in the right directory
if [ ! -f "Makefile" ]; then
    echo "Error: Must run from project root"
    exit 1
fi

echo "1. CHECKING EXISTING BINARIES"
echo "----------------------------------------"
ls -lh build/ 2>/dev/null | grep -v "^d" || echo "No binaries found"
echo ""

echo "2. SCANNING EXISTING DATA FILES"
echo "----------------------------------------"
if [ -f "data/raw_20251107_194636_ws0.ndjson" ]; then
    echo "Running: ./build/scan_recording data/raw_20251107_194636_ws0.ndjson"
    ./build/scan_recording data/raw_20251107_194636_ws0.ndjson | head -20
else
    echo "No data files found. Run stream_and_record first."
fi
echo ""

echo "3. LATENCY MEASUREMENT"
echo "----------------------------------------"
if [ -f "data/raw_20251107_194636_ws0.ndjson" ]; then
    echo "Running: ./build/complete_latency_test (first 1000 messages)"
    ./build/complete_latency_test data/raw_20251107_194636_ws0.ndjson 2>&1 | head -40
else
    echo "No data files for latency test"
fi
echo ""

echo "4. ZEROMQ PUB/SUB TEST"
echo "----------------------------------------"
echo "Running: ./build/test_zmq_pubsub"
timeout 3 ./build/test_zmq_pubsub 2>&1 || echo "(Test completed)"
echo ""

echo "5. PROJECT STRUCTURE"
echo "----------------------------------------"
echo "Core Components:"
find . -maxdepth 2 -name "*.hpp" -o -name "*.cpp" | grep -E "(zmq|proto|core|md|strats)" | head -20
echo ""

echo "6. DOCUMENTATION"
echo "----------------------------------------"
ls -lh *.md 2>/dev/null | awk '{print $9, "-", $5}'
echo ""

echo "=========================================="
echo "DEMO COMPLETE"
echo "=========================================="
echo ""
echo "Next steps:"
echo "  1. Review DEMO.md for complete documentation"
echo "  2. Check LATENCY_REPORT.md for performance analysis"
echo "  3. See PROJECT_STATUS.md for project overview"
echo ""
