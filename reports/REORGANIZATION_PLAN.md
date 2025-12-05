# Code Reorganization Plan

## Current Issues
1. Duplicate order_book.hpp in md/ and core/
2. Empty directories: pipeline/, indicators/, storage/, ml/
3. Mixed concerns in md/ directory
4. No clear separation between market data, storage, and processing

## Target Directory Structure

```
core/                   # Core primitives (keep existing)
├── order_book.hpp      # ✓ Use core version (more complete)
├── timestamp.hpp       # ✓ Keep
├── latency_tracker.hpp # ✓ Keep
└── metrics.hpp         # ✓ Keep

md/                     # Market data ONLY
├── ws_l2_client.hpp    # ✓ Keep (Coinbase client)
└── normalizer.hpp      # ✓ Keep (to be enhanced)

exchanges/              # Exchange-specific clients
├── exchange_config.hpp # ✓ Keep
├── coinbase_client.hpp # NEW (wrap ws_l2_client.hpp)
├── binance_client.hpp  # ✓ Keep
├── kraken_client.hpp   # NEW
└── okx_client.hpp      # NEW

pipeline/               # Data normalization & routing
├── normalized_data.hpp # NEW - Common data structures
├── normalizer.hpp      # NEW - Multi-exchange normalizer
├── queue_manager.hpp   # NEW - Lock-free queues
└── data_router.hpp     # NEW - Route to storage/strategies

storage/                # Persistence layer
├── inmem/
│   ├── quote_cache.hpp # NEW - Latest quotes per symbol
│   ├── tick_buffer.hpp # NEW - Ring buffer for ticks
│   └── ohlcv_cache.hpp # NEW - OHLCV bars
└── sqlite/
    ├── schema.sql      # NEW - DB schema
    ├── db_writer.hpp   # NEW - Async SQLite writer
    └── db_reader.hpp   # NEW - Query interface

indicators/             # Technical analysis
├── sma.hpp            # NEW - Simple moving average
├── ema.hpp            # NEW - Exponential moving average
├── rsi.hpp            # NEW - Relative strength index
├── macd.hpp           # NEW - MACD
├── bollinger.hpp      # NEW - Bollinger bands
├── vwap.hpp           # NEW - Volume-weighted average price
└── indicator_engine.hpp # NEW - Batch calculator

ml/                     # Machine learning
├── features/
│   ├── feature_extractor.hpp  # NEW - Extract features
│   ├── orderbook_features.hpp # NEW - Book imbalance, etc.
│   └── ta_features.hpp        # NEW - Technical indicators
├── models/
│   ├── model_interface.hpp    # NEW - Common interface
│   ├── linear_model.hpp       # NEW - Simple linear model
│   └── onnx_runner.hpp        # NEW - ONNX runtime wrapper
└── trainer/
    ├── dataset.hpp            # NEW - Training dataset
    └── online_learner.hpp     # NEW - Online learning

strats/                 # Trading strategies (keep all)
├── All existing strategy files ✓

arb/                    # Arbitrage (keep all)
├── All existing arb files ✓

bt/                     # Backtesting (keep)
└── backtester.hpp ✓

sim/                    # Simulation utilities (keep)
├── fx.hpp ✓
└── units.hpp ✓

io/                     # I/O utilities
├── recorder.hpp ✓
└── file_utils.hpp     # NEW

net/                    # Network utilities
└── http_client.hpp ✓

zmq/                    # ZeroMQ messaging (keep)
├── market_data_server.hpp ✓
└── market_data_client.hpp ✓

run/                    # Executables (keep all)
└── All existing ✓

proto/                  # Protocol buffers
└── (keep as is)

discovery/              # Service discovery
└── (keep as is)
```

## Actions
1. Remove md/order_book.hpp (duplicate, less complete)
2. Remove md/latency.hpp (duplicate of core/latency_tracker.hpp)
3. Create pipeline components
4. Create storage layer
5. Create indicators
6. Create ML infrastructure
7. Update includes in existing files
