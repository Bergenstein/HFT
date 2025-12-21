# System Testing Guide - Post Strategy Separation

## Architecture Overview

The system is now split into two repositories:
- **HFT_Full_Pipeline**: Core system (data pipeline, exchanges, queues, storage)
- **sabi-cppstrategies**: Strategies (arbitrage, ML, indicators, backtesting)

Communication happens via **ZeroMQ API** - strategies are blackbox binaries.

---

## Test 1: Build System Components

```bash
cd HFT_Full_Pipeline
make clean
make system
```

**Expected Output:**
```
✓ Built build/stream_and_record
✓ Built build/replay_and_book
✓ Built build/scan_recording
✓ Built build/complete_multi_exchange_pipeline
✓ Built build/production_hft_system
✓ Built build/test_hot_cold_integration
✓ Built build/full_system_integration
✓ Built build/dashboard_server
✓ Built build/backtest_from_sqlite
✓ Built build/backtest_to_json
```

---

## Test 2: System Pipeline Tests

```bash
cd HFT_Full_Pipeline
make tests
./build/test_system_pipeline
```

**Expected Output:**
```
==========================================================
  HFT SYSTEM PIPELINE TEST
==========================================================

[Test 1] Order Book Construction... ✓ PASS
[Test 2] Timestamp Generation... ✓ PASS
[Test 3] SPSC Queue Performance... ✓ PASS
[Test 4] Multi-threaded Data Flow... ✓ PASS

==========================================================
  TEST RESULTS
==========================================================
Passed: 4/4
Failed: 0

✅ All system tests passed!
```

---

## Test 3: Lock-Free Queue Performance

```bash
cd HFT_Full_Pipeline
./build/test_lockfree_queues
```

**Expected Output:**
```
Testing SPSC Queue...
Capacity: 1024
Operations: 100000
Time: ~42 ns per operation
✓ PASS

Testing MPMC Queue...
Producers: 4
Consumers: 4
Operations: 100000
Time: ~150 ns per operation
✓ PASS
```

---

## Test 4: Multi-Exchange Funding Rate System

```bash
cd HFT_Full_Pipeline
./build/test_multi_exchange_funding 30
```

**Expected Output:**
```
==========================================================
  MULTI-EXCHANGE FUNDING RATE TEST
==========================================================

✓ Connecting to 9 exchanges...
  - Binance (1000ms interval)
  - Bybit (1000ms interval)
  - Gate.io (1000ms interval)
  - OKX (2000ms interval)
  - MEXC (1000ms interval)
  - KuCoin (1000ms interval)
  - Kraken (1000ms interval)
  - Bitget (1000ms interval)
  - HTX (1000ms interval)

Running for 30 seconds...
[Updates: 1234 | With Funding: 891 | Coverage: 72.2%]

==========================================================
  FINAL RESULTS
==========================================================
Total Updates:        1234
Funding Rate Updates: 891
Coverage:             72.2%
Database Records:     1234

✓ Test completed successfully
```

---

## Test 5: Build Strategies (Separate Repo)

```bash
cd ../sabi-cppstrategies
make clean
make all
```

**Expected Output:**
```
✓ Built build/test_funding_arb_engine
✓ Built build/production_funding_arb
✓ Built build/backtest_strategy
✓ Built build/live_strategy_runner
✓ Built build/multi_exchange_arbitrage
...
```

---

## Test 6: Test Strategy with System (Blackbox Execution)

### Terminal 1: Start HFT System with API

```bash
cd HFT_Full_Pipeline
./build/production_hft_system 60
```

**Output:**
```
==========================================================
  HFT SYSTEM - MARKET DATA API SERVER
==========================================================

✓ API Server initialized
  Market Data: tcp://*:5555 (PUB)
  Signals:     tcp://*:5556 (PULL)

✓ API Server started

Starting multi-exchange data pipeline...
✓ Multi-exchange system started
  Exchanges: 9 (Binance, Bybit, Gate.io, OKX, MEXC, KuCoin, Kraken, Bitget, HTX)
  Database: db/hft_system_with_api.db

Running for 60 seconds...
Strategy binaries can connect to:
  - Subscribe: tcp://localhost:5555
  - Signal:    tcp://localhost:5556

[Updates: 2345 | API Publishes: 2345]
```

### Terminal 2: Run Strategy Binary (Blackbox)

```bash
cd ../sabi-cppstrategies
./build/production_funding_arb 60
```

**Output:**
```
==========================================================
  FUNDING RATE ARBITRAGE STRATEGY
==========================================================

✓ Connected to HFT System API
  Subscribe: tcp://localhost:5555
  Signal:    tcp://localhost:5556

Strategy ID: funding_arb_v1
Min Spread: 5.0% APY
Min Liquidity: 90%

Receiving market data...
[Data: 2345 | Opportunities: 12 | Signals Sent: 8]

━━━ OPPORTUNITY #1 ━━━━━━━━━━━━━━━━━━━━━━━━━━━
BTC/USDT - 8.55% APY
LONG:  Binance @ -2.10% APY (collecting)
SHORT: Bybit @ 11.65% APY (collecting)
Spread: 13.75% | Liquidity: $150000

✓ Signal sent: LONG BTC/USDT on Binance (0.1 BTC)
✓ Signal sent: SHORT BTC/USDT on Bybit (0.1 BTC)
```

### Terminal 1: System Receives Signals

```
[SIGNAL] Strategy: funding_arb_v1 | Action: LONG | Symbol: BTC/USDT | Qty: 0.1 | Exchange: Binance
[SIGNAL] Strategy: funding_arb_v1 | Action: SHORT | Symbol: BTC/USDT | Qty: 0.1 | Exchange: Bybit
```

---

## Test 7: Strategy Independence (Blackbox Verification)

The system should work with ANY strategy binary without knowing its internals:

```bash
# Terminal 1: System (unchanged)
cd HFT_Full_Pipeline
./build/production_hft_system 60

# Terminal 2: Strategy A
cd ../sabi-cppstrategies
./build/production_funding_arb 60

# Terminal 3: Strategy B (different strategy, same API)
cd ../sabi-cppstrategies
./build/live_strategy_runner imbalance 60

# Terminal 4: Strategy C (custom user strategy)
cd ../sabi-cppstrategies
./build/my_secret_strategy 60
```

All strategies receive the same market data feed via `tcp://localhost:5555`.
All strategies send signals via `tcp://localhost:5556`.
The system doesn't know what logic each strategy uses.

---

## Test 8: Latency Verification

```bash
cd HFT_Full_Pipeline
./build/test_system_pipeline
```

**Measure:**
- JSON parsing: ~3-4 μs (p50)
- Normalization: ~1-2 μs
- Order book update: ~1-2 μs
- SPSC queue transfer: ~42 ns
- ZeroMQ publish: ~100-200 ns

**Total end-to-end latency: ~7-15 microseconds**

---

## Test 9: Database Persistence

```bash
cd HFT_Full_Pipeline
sqlite3 db/hft_system_with_api.db << EOF
SELECT COUNT(*) as total_records FROM unified_market_data;
SELECT exchange, COUNT(*) as count 
FROM unified_market_data 
GROUP BY exchange 
ORDER BY count DESC;
EOF
```

**Expected Output:**
```
total_records
2345

exchange|count
Binance|345
Bybit|312
Gate.io|298
OKX|156
MEXC|245
KuCoin|234
Kraken|221
Bitget|267
HTX|267
```

---

## Test 10: Integration Test (Full Stack)

```bash
# This script tests the complete flow
cd ..
./test_integration.sh
```

**The script will:**
1. Start HFT system with API
2. Wait for initialization (5s)
3. Start 2 different strategy binaries
4. Run for 30 seconds
5. Verify data flow
6. Check database records
7. Validate signal reception
8. Clean shutdown

**Expected Output:**
```
==========================================================
  INTEGRATION TEST - SYSTEM + STRATEGIES
==========================================================

[1/8] Starting HFT System... ✓
[2/8] Waiting for initialization... ✓
[3/8] Starting Strategy A (funding_arb)... ✓
[4/8] Starting Strategy B (imbalance)... ✓
[5/8] Running for 30 seconds... ✓
[6/8] Verifying data flow... ✓
      System updates: 2345
      Strategy A received: 2345
      Strategy B received: 2345
[7/8] Checking database... ✓
      Records: 2345
[8/8] Validating signals... ✓
      Strategy A signals: 12
      Strategy B signals: 45

==========================================================
  ✅ INTEGRATION TEST PASSED
==========================================================
```

---

## Summary

### System Repository (HFT_Full_Pipeline)
- ✅ Builds without strategy dependencies
- ✅ Provides ZeroMQ API for market data
- ✅ Receives trading signals via API
- ✅ Stores data to SQLite
- ✅ No knowledge of strategy internals

### Strategy Repository (sabi-cppstrategies)
- ✅ Builds strategies as independent binaries
- ✅ Connects to system via ZeroMQ
- ✅ Receives market data feed
- ✅ Sends trading signals
- ✅ Fully blackbox from system perspective

### Communication
- ✅ Market Data: tcp://localhost:5555 (PUB/SUB)
- ✅ Signals: tcp://localhost:5556 (PUSH/PULL)
- ✅ JSON serialization
- ✅ Low latency (<15 μs end-to-end)
- ✅ Multi-strategy support

---

## Troubleshooting

### Issue: "Address already in use"
```bash
# Kill existing processes
pkill -f production_hft_system
pkill -f production_funding_arb
```

### Issue: "Connection refused"
```bash
# Ensure system is running first
cd HFT_Full_Pipeline
./build/production_hft_system 60 &
sleep 5
# Then start strategies
cd ../sabi-cppstrategies
./build/production_funding_arb 60
```

### Issue: "No market data received"
```bash
# Check if exchanges are accessible
curl -s https://api.binance.com/api/v3/ticker/price?symbol=BTCUSDT
# Verify ZeroMQ is working
netstat -an | grep 5555
```

---

## Next Steps

1. **Add more exchanges**: Edit `HFT_Full_Pipeline/exchanges/`
2. **Create new strategies**: Add to `sabi-cppstrategies/strats/`
3. **Deploy to production**: Use Docker containers
4. **Monitor performance**: Use dashboard at `http://localhost:8080`
