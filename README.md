# HFT Multi-Exchange Crypto Trading System

High-frequency trading system for cryptocurrency markets with sub-millisecond latency. Supports multiple exchanges, lock-free data structures, and funding rate arbitrage strategies.

---

## Quick Test (Copy-Paste)

```bash
# Build everything
make all

# Unit tests
./build/test_lockfree_queues
./build/test_matching_engine_simple

# Integration tests
./build/test_hot_cold_integration
./build/test_multi_exchange_hot_cold
./build/full_system_integration

# Funding rate backtest (generates JSON + dashboard)
./build/backtest_to_json BTCUSDT 1.0
./build/backtest_to_json ETHUSDT 1.0
./build/backtest_to_json SOLUSDT 1.0

# Live funding rate scanner
./build/real_funding_rate_arb BTCUSDT
./build/real_funding_rate_arb ETHUSDT

# Dashboard
python3 -m http.server 8080
open http://localhost:8080/dashboard/backtest_viewer.html
```

---

## Build

```bash
# Install dependencies (macOS)
brew install boost openssl@3 nlohmann-json zeromq cppzmq protobuf abseil websocketpp

# Build all binaries
make all

# Build specific binary
make build/<binary_name>
```

---

## Test Commands

### Queue Performance

```bash
./build/test_lockfree_queues
```
Tests SPSC and MPMC lock-free queue throughput. Measures messages per second for hot path data flow.

---

### Matching Engine

```bash
./build/test_matching_engine_simple
```
Tests order book operations: add, cancel, match. Verifies bid/ask sorting and trade execution logic.

---

### Hot/Cold Path Integration

```bash
./build/test_hot_cold_integration
```
Tests the separation between hot path (trading) and cold path (storage/metrics). Verifies SQLite persistence and memory queue handoff.

---

### Multi-Exchange Pipeline

```bash
./build/test_multi_exchange_hot_cold
```
Tests market data flow from multiple exchanges (Coinbase, Binance) through separate SPSC queues. Measures signal generation across exchanges.

---

### Full System Integration

```bash
./build/full_system_integration
```
End-to-end test of the complete trading pipeline: market data → order book → strategy → signals.

---

## Backtest Commands

### Funding Rate Arbitrage Backtest

```bash
./build/backtest_to_json <SYMBOL> <MIN_DIFF_BPS> [POSITION_SIZE] [OUTPUT_FILE]
```
```bash
./build/backtest_to_json ETHUSDT 1.0 2>&1
```
ls -la backtest_results.json

```bash 
./build/backtest_to_json BTCUSDT 1.0 2>&1
```
```
ls -la backtest_results.json && cat backtest_results.json | python3 -c "import json,sys; d=json.load(sys.stdin); print(f'Trades: {d[\"summary\"][\"total_trades\"]}, P&L: \${d[\"summary\"][\"total_pnl\"]:.2f}')"
```


| Parameter | Description | Default |
|-----------|-------------|---------|
| SYMBOL | Trading pair (BTCUSDT, ETHUSDT) | required |
| MIN_DIFF_BPS | Minimum funding rate differential (basis points) | required |
| POSITION_SIZE | Position size in USD | 100000 |
| OUTPUT_FILE | JSON output path | backtest_results.json |

**Example:**
```bash
./build/backtest_to_json BTCUSDT 1.0
./build/backtest_to_json ETHUSDT 2.0 50000 backtest_eth.json
```

Fetches historical funding rates from Binance, Bybit, OKX. Outputs JSON with equity curve and trade log.

---

### SQLite Backtest

```bash
./build/backtest_from_sqlite <DATABASE> <TABLE>
```
Runs backtest using recorded market data from SQLite database.

---

### Strategy Backtest

```bash
./build/backtest_strategy
```
Backtests order book imbalance strategy on recorded data.

---

## Live Commands

### Stream Market Data

```bash
./build/stream_and_record
```
Connects to exchange WebSockets and records market data to disk.

---

### Stream with Latency Measurement

```bash
./build/stream_with_latency
```
Same as above but logs latency metrics for each message.

---

### Replay Recorded Data

```bash
./build/replay_and_book
```
Replays recorded market data through order book reconstruction.

---

### Scan Recording

```bash
./build/scan_recording <FILE>
```
Inspects recorded market data file and prints statistics.

---

### Funding Rate Scanner

```bash
./build/real_funding_rate_arb <SYMBOL>
```
Fetches current funding rates from multiple exchanges and identifies arbitrage opportunities.

**Example:**
```bash
./build/real_funding_rate_arb BTCUSDT
```

```bash
(./build/real_funding_rate_arb BTCUSDT &); sleep 5; pkill -f real_funding_rate_arb 2>/dev/null; sleep 1
```


```bash
(./build/real_funding_rate_arb ETHUSDT &); sleep 5; pkill -f real_funding_rate_arb 2>/dev/null; sleep 1
```
---



## Dashboard

### Run Backtest and View Results

```bash
# Generate backtest results
./build/backtest_to_json BTCUSDT 1.0

# Start HTTP server
python3 -m http.server 8080

# Open dashboard
open http://localhost:8080/dashboard/backtest_viewer.html
```

---

## Docker

```bash
# Build image
docker build -t hft-system .

# Run container
docker run -it hft-system ./build/test_lockfree_queues

# Docker Compose
docker-compose up
```

---

## Project Structure

```
├── arb/          # Arbitrage strategies (funding rate, cross-exchange, statistical)
├── bt/           # Backtesting engine and data loaders
├── core/         # Order book, memory pool, latency tracking
├── dashboard/    # Web UI for backtest visualization
├── exchanges/    # Exchange connectors (Coinbase, Binance, etc.)
├── pipeline/     # Lock-free queues (SPSC, MPMC)
├── run/          # Executable entry points
├── strats/       # Trading strategies
├── storage/      # SQLite and file storage
└── zmq/          # ZeroMQ messaging for distributed components
```

---

## Data Sources

| Exchange | Funding Rate API | Limit |
|----------|------------------|-------|
| Binance | /fapi/v1/fundingRate | 1000 records |
| Bybit | /v5/market/funding/history | 200 records |
| OKX | /api/v5/public/funding-rate-history | 100 records |

---

## Fee Structure (VIP0)

| Exchange | Maker | Taker |
|----------|-------|-------|
| Binance | 0.02% | 0.04% |
| Bybit | 0.02% | 0.055% |
| OKX | 0.02% | 0.05% |

---

## Configuration

Configuration file: `config/system_config.json`

Environment variables for API keys:
- `COINBASE_API_KEY`
- `COINBASE_API_SECRET`
- `BINANCE_API_KEY`
- `BINANCE_API_SECRET`

---

## Performance Benchmarks

| Component | Throughput |
|-----------|------------|
| SPSC Queue | 24.5M msgs/sec |
| MPMC Queue | 5.9M msgs/sec |
| Order Book Update | < 1 μs |

---

## License

MIT


## Backtest:
./build/backtest_to_json BTCUSDT 1.0

ls -lh backtest_results.json

python3 -m http.server 8080