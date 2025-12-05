#pragma once

//==============================================================================
// HFT SYSTEM PUBLIC API
//==============================================================================
// This header defines the public API for packaging the HFT system.
// External consumers can use these interfaces to:
//   1. Integrate the trading engine into their applications
//   2. Access market data feeds
//   3. Execute strategies
//   4. Monitor performance
//
// Design Principles:
//   - Clean separation between interface and implementation
//   - No leaking internal dependencies
//   - Thread-safe by default
//   - Zero-copy where possible for latency
//
//==============================================================================

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <optional>

namespace hft {
namespace api {

//==============================================================================
// VERSION
//==============================================================================

struct Version {
    static constexpr int MAJOR = 1;
    static constexpr int MINOR = 0;
    static constexpr int PATCH = 0;
    static constexpr const char* STRING = "1.0.0";
};

//==============================================================================
// BASIC TYPES
//==============================================================================

using Timestamp = std::chrono::system_clock::time_point;
using Price = double;
using Quantity = double;
using OrderId = uint64_t;

enum class Side { BUY, SELL };
enum class OrderType { MARKET, LIMIT, STOP, STOP_LIMIT };
enum class OrderStatus { PENDING, OPEN, FILLED, PARTIALLY_FILLED, CANCELLED, REJECTED };
enum class Exchange { COINBASE, BINANCE, KRAKEN, OKX, BYBIT, HUOBI, DYDX };

//==============================================================================
// MARKET DATA STRUCTURES
//==============================================================================

struct PriceLevel {
    Price price;
    Quantity quantity;
};

struct Quote {
    std::string exchange;
    std::string symbol;
    Price bid_price;
    Price ask_price;
    Quantity bid_size;
    Quantity ask_size;
    Timestamp timestamp;
    uint64_t sequence;
};

struct OrderBookSnapshot {
    std::string exchange;
    std::string symbol;
    std::vector<PriceLevel> bids;  // Sorted descending by price
    std::vector<PriceLevel> asks;  // Sorted ascending by price
    Timestamp timestamp;
};

struct Trade {
    std::string exchange;
    std::string symbol;
    Price price;
    Quantity quantity;
    Side taker_side;
    Timestamp timestamp;
    std::string trade_id;
};

struct FundingRate {
    std::string exchange;
    std::string symbol;
    double rate;                   // 8-hour funding rate
    double predicted_rate;
    Timestamp next_funding_time;
    Timestamp timestamp;
};

//==============================================================================
// ORDER & POSITION STRUCTURES
//==============================================================================

struct Order {
    OrderId id;
    std::string exchange;
    std::string symbol;
    Side side;
    OrderType type;
    Price price;
    Quantity quantity;
    Quantity filled_quantity;
    OrderStatus status;
    Timestamp created_at;
    Timestamp updated_at;
    std::string client_order_id;
};

struct Position {
    std::string exchange;
    std::string symbol;
    Side side;
    Quantity quantity;
    Price avg_entry_price;
    Price mark_price;
    double unrealized_pnl;
    double realized_pnl;
    double liquidation_price;
    Timestamp opened_at;
};

struct Fill {
    std::string exchange;
    std::string symbol;
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;
    double fee;
    std::string fee_currency;
    Timestamp timestamp;
    std::string trade_id;
};

//==============================================================================
// STRATEGY INTERFACES
//==============================================================================

struct StrategySignal {
    std::string symbol;
    Side side;
    double strength;              // -1.0 to 1.0 (negative = sell, positive = buy)
    double confidence;            // 0.0 to 1.0
    Quantity suggested_quantity;
    Price limit_price;            // 0 = market order
    std::string reason;
    Timestamp timestamp;
};

struct StrategyMetrics {
    double total_pnl;
    double unrealized_pnl;
    double realized_pnl;
    int total_trades;
    int winning_trades;
    double win_rate;
    double sharpe_ratio;          // Daily (NOT annualized)
    double sortino_ratio;         // Daily (NOT annualized)
    double max_drawdown;
    double current_drawdown;
    double volatility;            // Daily
};

// Callback types
using QuoteCallback = std::function<void(const Quote&)>;
using TradeCallback = std::function<void(const Trade&)>;
using OrderBookCallback = std::function<void(const OrderBookSnapshot&)>;
using FundingCallback = std::function<void(const FundingRate&)>;
using FillCallback = std::function<void(const Fill&)>;
using SignalCallback = std::function<void(const StrategySignal&)>;

//==============================================================================
// CONFIGURATION
//==============================================================================

struct ExchangeConfig {
    Exchange exchange;
    std::string api_key;
    std::string api_secret;
    std::string passphrase;       // For OKX
    bool testnet = false;
    std::vector<std::string> symbols;
};

struct StrategyConfig {
    std::string name;
    std::string type;             // "funding_arb", "mean_reversion", etc.
    std::vector<std::string> symbols;
    double max_position_usd = 100000;
    double risk_per_trade_pct = 1.0;
    std::map<std::string, double> parameters;
};

struct SystemConfig {
    std::vector<ExchangeConfig> exchanges;
    std::vector<StrategyConfig> strategies;
    
    std::string data_dir = "./data";
    std::string log_level = "INFO";
    bool enable_trading = false;  // Paper trading by default
    
    int hot_path_cpu_core = -1;   // -1 = no pinning
    int cold_path_cpu_core = -1;
    
    std::string zmq_metrics_endpoint = "tcp://*:5555";
    std::string zmq_signals_endpoint = "tcp://*:5556";
};

//==============================================================================
// MARKET DATA FEED INTERFACE
//==============================================================================

class IMarketDataFeed {
public:
    virtual ~IMarketDataFeed() = default;
    
    // Connection management
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    
    // Subscription
    virtual bool subscribe(const std::vector<std::string>& symbols) = 0;
    virtual bool unsubscribe(const std::vector<std::string>& symbols) = 0;
    
    // Callbacks
    virtual void set_quote_callback(QuoteCallback cb) = 0;
    virtual void set_trade_callback(TradeCallback cb) = 0;
    virtual void set_orderbook_callback(OrderBookCallback cb) = 0;
    virtual void set_funding_callback(FundingCallback cb) = 0;
    
    // Snapshot access
    virtual std::optional<Quote> get_quote(const std::string& symbol) const = 0;
    virtual std::optional<OrderBookSnapshot> get_orderbook(const std::string& symbol, int depth = 10) const = 0;
};

//==============================================================================
// ORDER EXECUTION INTERFACE
//==============================================================================

class IOrderExecutor {
public:
    virtual ~IOrderExecutor() = default;
    
    // Order management
    virtual OrderId submit_order(const Order& order) = 0;
    virtual bool cancel_order(OrderId id) = 0;
    virtual bool cancel_all_orders(const std::string& symbol = "") = 0;
    virtual bool modify_order(OrderId id, Price new_price, Quantity new_qty) = 0;
    
    // Query
    virtual std::optional<Order> get_order(OrderId id) const = 0;
    virtual std::vector<Order> get_open_orders(const std::string& symbol = "") const = 0;
    virtual std::vector<Fill> get_fills(Timestamp since) const = 0;
    
    // Position
    virtual std::optional<Position> get_position(const std::string& symbol) const = 0;
    virtual std::vector<Position> get_all_positions() const = 0;
    
    // Callbacks
    virtual void set_fill_callback(FillCallback cb) = 0;
};

//==============================================================================
// STRATEGY ENGINE INTERFACE
//==============================================================================

class IStrategy {
public:
    virtual ~IStrategy() = default;
    
    virtual std::string name() const = 0;
    virtual void initialize(const StrategyConfig& config) = 0;
    
    // Called on each market data update
    virtual void on_quote(const Quote& quote) = 0;
    virtual void on_trade(const Trade& trade) = 0;
    virtual void on_orderbook(const OrderBookSnapshot& book) = 0;
    virtual void on_funding(const FundingRate& funding) = 0;
    
    // Called on each fill
    virtual void on_fill(const Fill& fill) = 0;
    
    // Get current signal
    virtual std::optional<StrategySignal> get_signal() const = 0;
    
    // Metrics
    virtual StrategyMetrics get_metrics() const = 0;
    
    // Lifecycle
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};

//==============================================================================
// BACKTESTER INTERFACE
//==============================================================================

struct BacktestConfig {
    Timestamp start_time;
    Timestamp end_time;
    double initial_capital = 100000;
    double fee_rate_bps = 4.0;
    std::string data_source;      // SQLite path or data provider
};

struct BacktestResult {
    StrategyMetrics metrics;
    std::vector<Fill> trades;
    std::vector<double> equity_curve;
    std::vector<double> drawdown_curve;
    Timestamp start_time;
    Timestamp end_time;
    int total_bars;
};

class IBacktester {
public:
    virtual ~IBacktester() = default;
    
    virtual bool load_data(const std::string& source) = 0;
    virtual BacktestResult run(IStrategy& strategy, const BacktestConfig& config) = 0;
    virtual void export_results(const std::string& path, const BacktestResult& result) = 0;
};

//==============================================================================
// MAIN TRADING ENGINE INTERFACE
//==============================================================================

class ITradingEngine {
public:
    virtual ~ITradingEngine() = default;
    
    // Initialization
    virtual bool initialize(const SystemConfig& config) = 0;
    virtual void shutdown() = 0;
    
    // Exchange access
    virtual IMarketDataFeed* get_market_data(Exchange exchange) = 0;
    virtual IOrderExecutor* get_executor(Exchange exchange) = 0;
    
    // Strategy management
    virtual bool register_strategy(std::unique_ptr<IStrategy> strategy) = 0;
    virtual IStrategy* get_strategy(const std::string& name) = 0;
    virtual std::vector<std::string> get_strategy_names() const = 0;
    
    // Control
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
    
    // Signal access
    virtual void set_signal_callback(SignalCallback cb) = 0;
};

//==============================================================================
// FACTORY FUNCTIONS
//==============================================================================

// Create trading engine instance
std::unique_ptr<ITradingEngine> create_trading_engine();

// Create market data feed for specific exchange
std::unique_ptr<IMarketDataFeed> create_market_data_feed(const ExchangeConfig& config);

// Create order executor for specific exchange
std::unique_ptr<IOrderExecutor> create_order_executor(const ExchangeConfig& config);

// Create backtester
std::unique_ptr<IBacktester> create_backtester();

// Create built-in strategies
std::unique_ptr<IStrategy> create_funding_rate_arb_strategy();
std::unique_ptr<IStrategy> create_mean_reversion_strategy();
std::unique_ptr<IStrategy> create_momentum_strategy();
std::unique_ptr<IStrategy> create_imbalance_strategy();
std::unique_ptr<IStrategy> create_cross_exchange_arb_strategy();

//==============================================================================
// UTILITY FUNCTIONS
//==============================================================================

// Symbol normalization
std::string normalize_symbol(const std::string& symbol, Exchange exchange);
std::string denormalize_symbol(const std::string& normalized, Exchange exchange);

// Exchange utilities
std::string exchange_to_string(Exchange exchange);
Exchange string_to_exchange(const std::string& str);

// Timestamp utilities
Timestamp parse_timestamp(const std::string& str);
std::string format_timestamp(Timestamp ts);

} // namespace api
} // namespace hft
