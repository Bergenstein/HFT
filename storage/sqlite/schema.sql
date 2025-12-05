-- storage/sqlite/schema.sql - HFT Market Data Schema
-- Optimized for high-frequency writes and fast queries

-- Quotes table (L1 best bid/ask)
CREATE TABLE IF NOT EXISTS quotes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    exchange TEXT NOT NULL,
    product_id TEXT NOT NULL,
    timestamp INTEGER NOT NULL,  -- Unix microseconds
    best_bid REAL NOT NULL,
    best_ask REAL NOT NULL,
    bid_size REAL NOT NULL,
    ask_size REAL NOT NULL,
    sequence INTEGER,
    latency_us INTEGER
);

CREATE INDEX idx_quotes_exchange_product_time ON quotes(exchange, product_id, timestamp);
CREATE INDEX idx_quotes_time ON quotes(timestamp);

-- Trades table
CREATE TABLE IF NOT EXISTS trades (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    exchange TEXT NOT NULL,
    product_id TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    price REAL NOT NULL,
    size REAL NOT NULL,
    side TEXT NOT NULL,  -- 'buy' or 'sell'
    trade_id TEXT,
    sequence INTEGER
);

CREATE INDEX idx_trades_exchange_product_time ON trades(exchange, product_id, timestamp);

-- OHLCV bars (aggregated)
CREATE TABLE IF NOT EXISTS ohlcv (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    exchange TEXT NOT NULL,
    product_id TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    interval_sec INTEGER NOT NULL,  -- 60=1min, 300=5min, etc
    open REAL NOT NULL,
    high REAL NOT NULL,
    low REAL NOT NULL,
    close REAL NOT NULL,
    volume REAL NOT NULL,
    num_trades INTEGER
);

CREATE UNIQUE INDEX idx_ohlcv_unique ON ohlcv(exchange, product_id, timestamp, interval_sec);

-- Arbitrage opportunities (for analysis)
CREATE TABLE IF NOT EXISTS arbitrage_opportunities (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    arb_type TEXT NOT NULL,  -- 'cross_exchange', 'triangular', 'statistical'
    product_id TEXT NOT NULL,
    buy_exchange TEXT,
    sell_exchange TEXT,
    buy_price REAL,
    sell_price REAL,
    gross_spread_bps REAL,
    net_spread_bps REAL,
    expected_profit_usd REAL,
    confidence REAL,
    executable INTEGER  -- 0 or 1
);

CREATE INDEX idx_arb_time ON arbitrage_opportunities(timestamp);
CREATE INDEX idx_arb_product ON arbitrage_opportunities(product_id);

-- Exchange status tracking
CREATE TABLE IF NOT EXISTS exchange_status (
    exchange TEXT PRIMARY KEY,
    last_update INTEGER NOT NULL,
    is_connected INTEGER NOT NULL,  -- 0 or 1
    total_quotes INTEGER DEFAULT 0,
    total_trades INTEGER DEFAULT 0,
    avg_latency_us INTEGER
);
