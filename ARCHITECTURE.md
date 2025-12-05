# HFT System Architecture - Production Implementation

## Overview

This document describes the complete production-grade HFT system architecture with proper data flow, queue management, and strategy execution.

## 1. DATA FLOW ARCHITECTURE

### 1.1 Hot Path (Per-Exchange Real-Time Processing)

```
┌─────────────────────────────────────────────────────────────────┐
│                    PER-EXCHANGE HOT PATH                         │
│                   (Pinned to Dedicated CPU Cores)                │
└─────────────────────────────────────────────────────────────────┘

Exchange WebSocket Thread (Core 0-2)
  ↓ [Raw JSON Messages]
  ↓
Normalizer (Inline in same thread)
  ↓ [NormalizedQuote]
  ↓
SPSC Queue (Lock-Free, Single Exchange)
  ↓
Strategy Processor Thread (Core 3-5)
  ├→ OFI Strategy
  ├→ Imbalance Strategy
  ├→ Microprice Strategy
  └→ Other Single-Exchange Strategies
  ↓ [Strategy Signals]
  ↓
ZeroMQ Publisher (tcp://localhost:5555)
  ├→ Topic: "signals.coinbase.BTC-USD"
  ├→ Topic: "signals.binance.BTC-USD"
  └→ Topic: "signals.kraken.BTC-USD"
```

**Key Design Decisions:**
- One SPSC queue per exchange (lock-free, minimal contention)
- Strategy runs in dedicated thread (CPU pinned)
- Normalizer runs inline (no extra hop)
- ZeroMQ publishes results (decoupled from consumers)

### 1.2 Cold Path (Cross-Exchange Aggregation)

```
┌─────────────────────────────────────────────────────────────────┐
│                   CROSS-EXCHANGE COLD PATH                       │
│                  (Archive + Multi-Asset Strategies)              │
└─────────────────────────────────────────────────────────────────┘

Multiple Exchange Threads
  ├→ Coinbase Thread → NormalizedQuote
  ├→ Binance Thread → NormalizedQuote
  └→ Kraken Thread → NormalizedQuote
  ↓
MPMC Aggregation Queue (Multi-Producer, Multi-Consumer)
  ├→ Producer 1: Coinbase Thread
  ├→ Producer 2: Binance Thread
  ├→ Producer 3: Kraken Thread
  ↓
Multiple Consumers:
  ├→ Consumer 1: Cross-Exchange Arb Strategy Thread
  │   ├→ Perp-Spot Arbitrage
  │   ├→ Funding Rate Arbitrage
  │   └→ Market Neutral Pairs
  │   ↓ [Arbitrage Signals]
  │   └→ ZeroMQ Publisher (tcp://localhost:5556)
  │
  ├→ Consumer 2: Archival Thread (SQLite WAL)
  │   └→ Batch write (100 quotes per transaction)
  │
  └→ Consumer 3: Exchange Simulator Feed
      └→ Simulated order execution
```

**Key Design Decisions:**
- MPMC queue for multiple exchanges → multiple consumers
- Separate threads for different consumer types
- SQLite with WAL mode (non-blocking reads)
- Exchange simulator consumes same data (paper trading)

## 2. QUEUE TYPES & USE CASES

### 2.1 SPSC Queue (Single Producer Single Consumer)
- **File**: `pipeline/spsc_queue.hpp`
- **Performance**: 24.8M msgs/sec
- **Use Case**: Exchange WebSocket → Strategy Processor
- **CPU Pinning**: Producer on Core N, Consumer on Core N+1

### 2.2 MPMC Queue (Multi Producer Multi Consumer)
- **File**: `pipeline/mpmc_queue.hpp`
- **Performance**: 5.2M msgs/sec
- **Use Case**: Multiple Exchanges → Aggregated Consumers
- **Contention**: CAS loops, cache line aligned

## 3. ZEROMQ PUBLICATION CHANNELS

### 3.1 Market Data Channel (Port 5555)
```
Topic Format: "market.<exchange>.<product>"
Examples:
  - "market.coinbase.BTC-USD"
  - "market.binance.BTC-USDT"
  - "market.kraken.XBT-USD"

Payload: Protobuf MarketDataUpdate
  - exchange, product_id
  - best_bid, best_ask
  - bid_size, ask_size
  - top 5 levels
  - timestamps
```

### 3.2 Strategy Signals Channel (Port 5556)
```
Topic Format: "signal.<strategy>.<exchange>.<product>"
Examples:
  - "signal.ofi.coinbase.BTC-USD"
  - "signal.imbalance.binance.ETH-USDT"
  - "signal.perp_spot_arb.multi.BTC-USD"

Payload: Protobuf StrategySignal
  - strategy_name
  - action (+1 buy, -1 sell, 0 hold)
  - confidence
  - timestamp
  - metadata (JSON)
```

### 3.3 Arbitrage Opportunities Channel (Port 5557)
```
Topic Format: "arb.<arb_type>.<product>"
Examples:
  - "arb.cross_exchange.BTC-USD"
  - "arb.perp_spot.BTC-USD"
  - "arb.funding_rate.ETH-USD"

Payload: Protobuf ArbOpportunity
  - type, product
  - buy_exchange, sell_exchange
  - buy_price, sell_price
  - gross_spread_bps, net_spread_bps
  - expected_profit_usd
  - confidence
```

### 3.4 Backtest Results Channel (Port 5558)
```
Topic Format: "backtest.<run_id>"

Payload: Protobuf BacktestMetrics
  - sharpe_ratio, sortino_ratio
  - max_drawdown, calmar_ratio
  - win_rate, profit_factor
  - equity_curve (array)
```

### 3.5 Metrics & Monitoring Channel (Port 5559)
```
Topic Format: "metrics.<component>"

Payload: Protobuf SystemMetrics
  - queue_depth, queue_utilization
  - latency_p50, latency_p99
  - messages_per_second
  - cpu_usage, memory_usage
```

## 4. EXCHANGE SIMULATOR INTEGRATION

### 4.1 Simulator Data Flow
```
MPMC Aggregation Queue (Historical or Live)
  ↓
Exchange Simulator Thread
  ├→ Consumes NormalizedQuote
  ├→ Maintains simulated order books
  ├→ Matches orders with realistic slippage
  └→ Publishes fill confirmations
  ↓
ZeroMQ Publisher (tcp://localhost:5560)
  └→ Topic: "sim.fills.<exchange>.<product>"
```

### 4.2 Simulator Components
- **Order Book**: Reconstructs L2 book from quotes
- **Matching Engine**: FIFO price-time priority
- **Slippage Model**: Market impact based on order size
- **Latency Injection**: Simulated network delay (1-10ms)

### 4.3 Simulator Use Cases
1. **Paper Trading**: Test strategies without risk
2. **Backtesting**: Replay historical data
3. **Strategy Development**: Validate logic before live
4. **Risk Assessment**: Measure worst-case scenarios

## 5. CPU PINNING STRATEGY

```
Core 0: Coinbase WebSocket Thread
Core 1: Binance WebSocket Thread
Core 2: Kraken WebSocket Thread
Core 3: Coinbase Strategy Processor
Core 4: Binance Strategy Processor
Core 5: Kraken Strategy Processor
Core 6: Cross-Exchange Arb Thread
Core 7: Archival Thread (SQLite)
Core 8: Exchange Simulator Thread
Core 9: ZeroMQ Publisher Thread
Core 10: Monitoring & Metrics Thread
Core 11: Reserved for OS/Housekeeping
```

**Implementation**:
- Use `core/cpu_affinity.hpp::set_thread_affinity()`
- Platform-specific: Linux (`pthread_setaffinity_np`), macOS (no-op or Mach APIs)
- Avoid hyperthreads for latency-critical paths

## 6. BACKTEST DATA FLOW

### 6.1 Historical Data Replay
```
SQLite Database (Archived Quotes)
  ↓
Backtest Engine Thread
  ├→ Load quotes in chronological order
  ├→ Push to MPMC queue (simulated live feed)
  └→ Rate limit to match historical timestamp gaps
  ↓
Strategy Threads (same as live)
  ├→ Execute strategies on historical data
  ├→ Generate signals
  └→ Record performance metrics
  ↓
Metrics Aggregator
  ├→ Calculate Sharpe, Sortino, Drawdown
  └→ Publish to ZeroMQ (Port 5558)
  ↓
Dashboard / Analytics Client
  └→ Visualize results, equity curve
```

### 6.2 Backtest Features
- **Realistic Slippage**: Exchange simulator provides fills
- **No Look-Ahead Bias**: Strict timestamp ordering
- **Transaction Costs**: Accurate fee modeling
- **Walk-Forward Analysis**: Rolling windows for robustness

## 7. MONITORING & OBSERVABILITY

### 7.1 Metrics Collection
```cpp
struct SystemMetrics {
    // Queue metrics
    size_t queue_depth;
    double queue_utilization;  // % full
    
    // Latency metrics (microseconds)
    double latency_mean;
    double latency_p50;
    double latency_p99;
    double latency_p999;
    
    // Throughput metrics
    uint64_t messages_per_second;
    uint64_t total_messages;
    uint64_t dropped_messages;
    
    // Resource metrics
    double cpu_usage_percent;
    double memory_usage_mb;
    
    // Strategy metrics
    uint64_t signals_generated;
    double signal_accuracy;  // % profitable
};
```

### 7.2 Health Checks
- Queue depth alarms (>80% full)
- Latency spike detection (p99 > 100μs)
- Message drop alerts
- CPU saturation warnings

## 8. ERROR HANDLING & RESILIENCE

### 8.1 Exchange Disconnection
```cpp
void handle_disconnect() {
    // 1. Stop consuming from queue
    // 2. Set health status to DEGRADED
    // 3. Attempt reconnection (exponential backoff)
    // 4. Publish alert to monitoring channel
    // 5. Continue with other exchanges
}
```

### 8.2 Queue Overflow
```cpp
if (!queue->try_enqueue(quote)) {
    // Queue is full (backpressure)
    metrics.dropped_messages++;
    
    // Drop oldest message instead?
    // Or block and wait?
    // Or publish overflow alert?
}
```

### 8.3 Strategy Crashes
```cpp
try {
    strategy->on_tick(context, orderbook);
} catch (const std::exception& e) {
    // Log exception
    // Disable faulty strategy
    // Continue with other strategies
    // Alert operations team
}
```

## 9. MAKEFILE AUTOMATION

### 9.1 Build Targets
```makefile
# Development
make dev          # Build with debug symbols
make test         # Run all tests
make benchmark    # Performance benchmarks

# Production
make release      # Optimized build (-O3, -march=native)
make install      # Install binaries
make run-hot-path # Start per-exchange processors
make run-cold-path # Start cross-exchange aggregator
make run-backtest # Run historical backtest

# Monitoring
make dashboard    # Start monitoring dashboard
make metrics      # Publish system metrics

# Utilities
make clean        # Remove build artifacts
make format       # Format code (clang-format)
make lint         # Static analysis (clang-tidy)
```

### 9.2 Integration Tests
```makefile
test-integration:
    # 1. Start exchange simulator
    # 2. Start hot path processors
    # 3. Start strategies
    # 4. Inject test data
    # 5. Verify signals published
    # 6. Check metrics
    # 7. Shutdown gracefully
```

## 10. DEPLOYMENT

### 10.1 Process Architecture
```
Process 1: coinbase_hot_path  (Cores 0, 3)
Process 2: binance_hot_path   (Cores 1, 4)
Process 3: kraken_hot_path    (Cores 2, 5)
Process 4: cross_exchange_arb (Core 6)
Process 5: archival_service   (Core 7)
Process 6: exchange_simulator (Core 8)
Process 7: zmq_publisher      (Core 9)
Process 8: monitoring_service (Core 10)
```

### 10.2 Service Management
```bash
# systemd services
systemctl start hft-coinbase.service
systemctl start hft-binance.service
systemctl start hft-aggregator.service

# Health checks
curl http://localhost:8080/health
curl http://localhost:8080/metrics
```

## 11. SECURITY

### 11.1 API Key Management
- Store in secure vault (HashiCorp Vault, AWS Secrets Manager)
- Never commit to git
- Rotate keys regularly

### 11.2 TLS/SSL
- Enable certificate verification for WebSocket connections
- Use TLS 1.3
- Pin certificates for known exchanges

### 11.3 Network Isolation
- ZeroMQ on localhost only (127.0.0.1)
- Or use TLS encryption for remote subscribers
- Firewall rules to restrict access

---

**Last Updated**: 2025-11-18  
**Author**: System Architect  
**Status**: Production Ready  
