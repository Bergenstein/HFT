// ============================================================================
// run/backtest_from_sqlite.cpp - Backtest using REAL historical data from SQLite
// ============================================================================
//
// This backtester uses ONLY real recorded market data from the SQLite database.
// NO simulated data is used.
//
// Build:
//   make build/backtest_from_sqlite
//
// Run:
//   ./build/backtest_from_sqlite demo_market_data.db BTC-USDT
//
// ============================================================================

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iomanip>

#include "../bt/sqlite_data_loader.hpp"
#include "../core/order_book.hpp"
#include "../core/metrics.hpp"

// ============================================================================
// Strategy Interface
// ============================================================================

struct BacktestTick {
    int64_t timestamp_us;
    double best_bid;
    double best_ask;
    double bid_size;
    double ask_size;
    double mid;
    double spread_bps;
    double imbalance;  // (bid_size - ask_size) / (bid_size + ask_size)
};

class BacktestStrategy {
public:
    virtual ~BacktestStrategy() = default;
    
    // Returns: +1 = buy, -1 = sell, 0 = hold
    virtual int on_tick(const BacktestTick& tick) = 0;
    virtual std::string name() const = 0;
};

// ============================================================================
// Example Strategy: Imbalance-based
// ============================================================================

class ImbalanceStrategy : public BacktestStrategy {
public:
    ImbalanceStrategy(double threshold = 0.3) : threshold_(threshold) {}
    
    int on_tick(const BacktestTick& tick) override {
        // Buy when bid size >> ask size (strong buying pressure)
        // Sell when ask size >> bid size (strong selling pressure)
        if (tick.imbalance > threshold_) return 1;   // Buy
        if (tick.imbalance < -threshold_) return -1; // Sell
        return 0;  // Hold
    }
    
    std::string name() const override { return "Imbalance(thresh=" + std::to_string(threshold_) + ")"; }
    
private:
    double threshold_;
};

// ============================================================================
// Example Strategy: Mean Reversion
// ============================================================================

class MeanReversionStrategy : public BacktestStrategy {
public:
    MeanReversionStrategy(int window = 50, double z_threshold = 2.0) 
        : window_(window), z_threshold_(z_threshold) {}
    
    int on_tick(const BacktestTick& tick) override {
        mids_.push_back(tick.mid);
        if (mids_.size() > static_cast<size_t>(window_)) {
            mids_.erase(mids_.begin());
        }
        
        if (mids_.size() < static_cast<size_t>(window_)) return 0;
        
        // Calculate mean and std
        double sum = std::accumulate(mids_.begin(), mids_.end(), 0.0);
        double mean = sum / mids_.size();
        
        double sq_sum = 0;
        for (double m : mids_) {
            sq_sum += (m - mean) * (m - mean);
        }
        double std_dev = std::sqrt(sq_sum / mids_.size());
        
        if (std_dev < 1e-9) return 0;
        
        double z_score = (tick.mid - mean) / std_dev;
        
        // Mean reversion: buy when price is low, sell when high
        if (z_score < -z_threshold_) return 1;   // Buy (price below mean)
        if (z_score > z_threshold_) return -1;   // Sell (price above mean)
        return 0;
    }
    
    std::string name() const override { 
        return "MeanReversion(window=" + std::to_string(window_) + ",z=" + std::to_string(z_threshold_) + ")"; 
    }
    
private:
    int window_;
    double z_threshold_;
    std::vector<double> mids_;
};

// ============================================================================
// Example Strategy: Momentum
// ============================================================================

class MomentumStrategy : public BacktestStrategy {
public:
    MomentumStrategy(int lookback = 20, double threshold = 0.001) 
        : lookback_(lookback), threshold_(threshold) {}
    
    int on_tick(const BacktestTick& tick) override {
        mids_.push_back(tick.mid);
        if (mids_.size() > static_cast<size_t>(lookback_)) {
            mids_.erase(mids_.begin());
        }
        
        if (mids_.size() < static_cast<size_t>(lookback_)) return 0;
        
        // Calculate momentum as percentage change
        double old_mid = mids_.front();
        double momentum = (tick.mid - old_mid) / old_mid;
        
        // Follow momentum
        if (momentum > threshold_) return 1;   // Buy (upward momentum)
        if (momentum < -threshold_) return -1; // Sell (downward momentum)
        return 0;
    }
    
    std::string name() const override { 
        return "Momentum(lookback=" + std::to_string(lookback_) + ",thresh=" + std::to_string(threshold_) + ")"; 
    }
    
private:
    int lookback_;
    double threshold_;
    std::vector<double> mids_;
};

// ============================================================================
// Backtest Engine
// ============================================================================

struct BacktestResult {
    std::string strategy_name;
    std::string product_id;
    int64_t total_ticks;
    int64_t total_trades;
    double initial_capital;
    double final_equity;
    double net_pnl;
    double net_pnl_pct;
    double total_fees;
    double sharpe_ratio;
    double sortino_ratio;
    double max_drawdown;
    double win_rate;
};

class SQLiteBacktester {
public:
    SQLiteBacktester(
        double initial_capital = 50000.0,
        double position_size = 0.01,
        double fee_bps = 10.0
    ) : initial_capital_(initial_capital), 
        position_size_(position_size),
        fee_bps_(fee_bps) {}
    
    BacktestResult run(
        const std::vector<bt::HistoricalQuote>& quotes,
        BacktestStrategy& strategy
    ) {
        BacktestResult result;
        result.strategy_name = strategy.name();
        result.product_id = quotes.empty() ? "" : quotes.front().product_id;
        result.initial_capital = initial_capital_;
        result.total_ticks = static_cast<int64_t>(quotes.size());
        
        // State
        double cash = initial_capital_;
        double position = 0.0;
        double total_fees = 0.0;
        int trades = 0;
        int wins = 0;
        double last_trade_price = 0.0;
        
        std::vector<double> equity_curve;
        std::vector<double> returns;
        
        equity_curve.push_back(initial_capital_);
        
        for (const auto& q : quotes) {
            BacktestTick tick;
            tick.timestamp_us = q.timestamp_us;
            tick.best_bid = q.best_bid;
            tick.best_ask = q.best_ask;
            tick.bid_size = q.bid_size;
            tick.ask_size = q.ask_size;
            tick.mid = q.mid();
            tick.spread_bps = q.spread_bps();
            double total_size = q.bid_size + q.ask_size;
            tick.imbalance = total_size > 0 ? (q.bid_size - q.ask_size) / total_size : 0;
            
            // Get signal
            int signal = strategy.on_tick(tick);
            
            // Execute trades
            if (signal != 0) {
                double trade_price = (signal > 0) ? tick.best_ask : tick.best_bid;
                double trade_qty = position_size_;
                double trade_value = trade_price * trade_qty;
                double fee = trade_value * fee_bps_ / 10000.0;
                
                if (signal > 0 && position <= 0) {
                    // Buy
                    if (cash >= trade_value + fee) {
                        cash -= trade_value + fee;
                        position += trade_qty;
                        total_fees += fee;
                        trades++;
                        
                        // Track P&L for win rate
                        if (last_trade_price > 0 && position == 0) {
                            // Closed position
                            if (trade_price > last_trade_price) wins++;
                        }
                        last_trade_price = trade_price;
                    }
                } else if (signal < 0 && position >= 0) {
                    // Sell
                    if (position > 0) {
                        cash += trade_value - fee;
                        position -= trade_qty;
                        total_fees += fee;
                        trades++;
                        
                        // Track P&L for win rate
                        if (trade_price > last_trade_price) wins++;
                        last_trade_price = trade_price;
                    }
                }
            }
            
            // Calculate equity
            double equity = cash + position * tick.mid;
            
            // Track returns
            if (!equity_curve.empty()) {
                double prev_equity = equity_curve.back();
                if (prev_equity > 0) {
                    returns.push_back((equity - prev_equity) / prev_equity);
                }
            }
            equity_curve.push_back(equity);
        }
        
        // Final mark-to-market
        double final_equity = cash;
        if (position != 0 && !quotes.empty()) {
            final_equity += position * quotes.back().mid();
        }
        
        // Calculate metrics
        result.final_equity = final_equity;
        result.net_pnl = final_equity - initial_capital_;
        result.net_pnl_pct = (result.net_pnl / initial_capital_) * 100.0;
        result.total_fees = total_fees;
        result.total_trades = trades;
        result.win_rate = trades > 0 ? (static_cast<double>(wins) / trades) * 100.0 : 0.0;
        
        // Sharpe ratio (assuming returns are per-tick, annualize roughly)
        if (!returns.empty()) {
            double sum = std::accumulate(returns.begin(), returns.end(), 0.0);
            double mean_ret = sum / returns.size();
            double sq_sum = 0;
            for (double r : returns) {
                sq_sum += (r - mean_ret) * (r - mean_ret);
            }
            double std_ret = std::sqrt(sq_sum / returns.size());
            result.sharpe_ratio = std_ret > 0 ? (mean_ret / std_ret) * std::sqrt(252.0 * 24 * 60) : 0;
            
            // Sortino ratio (downside deviation)
            double sq_down = 0;
            int down_count = 0;
            for (double r : returns) {
                if (r < 0) {
                    sq_down += r * r;
                    down_count++;
                }
            }
            double downside_dev = down_count > 0 ? std::sqrt(sq_down / down_count) : 0;
            result.sortino_ratio = downside_dev > 0 ? (mean_ret / downside_dev) * std::sqrt(252.0 * 24 * 60) : 0;
        }
        
        // Max drawdown
        double peak = initial_capital_;
        double max_dd = 0;
        for (double eq : equity_curve) {
            if (eq > peak) peak = eq;
            double dd = (peak - eq) / peak;
            if (dd > max_dd) max_dd = dd;
        }
        result.max_drawdown = max_dd * 100.0;
        
        return result;
    }
    
private:
    double initial_capital_;
    double position_size_;
    double fee_bps_;
};

// ============================================================================
// Main
// ============================================================================

void print_result(const BacktestResult& r) {
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                   BACKTEST RESULTS                            ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Strategy:       " << r.strategy_name << "\n";
    std::cout << "║ Product:        " << r.product_id << "\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Total Ticks:    " << r.total_ticks << "\n";
    std::cout << "║ Total Trades:   " << r.total_trades << "\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "║ Initial Capital: $" << r.initial_capital << "\n";
    std::cout << "║ Final Equity:    $" << r.final_equity << "\n";
    std::cout << "║ Net P&L:         $" << r.net_pnl << " (" << r.net_pnl_pct << "%)\n";
    std::cout << "║ Total Fees:      $" << r.total_fees << "\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Sharpe Ratio:    " << r.sharpe_ratio << "\n";
    std::cout << "║ Sortino Ratio:   " << r.sortino_ratio << "\n";
    std::cout << "║ Max Drawdown:    " << r.max_drawdown << "%\n";
    std::cout << "║ Win Rate:        " << r.win_rate << "%\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
}

int main(int argc, char** argv) {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║     SQLite Backtester - Using REAL Historical Data           ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    
    // Parse arguments
    std::string db_path = "demo_market_data.db";
    std::string product_id = "BTC-USDT";
    
    if (argc >= 2) db_path = argv[1];
    if (argc >= 3) product_id = argv[2];
    
    std::cout << "Database: " << db_path << "\n";
    std::cout << "Product:  " << product_id << "\n\n";
    
    try {
        // Load data from SQLite
        bt::SQLiteDataLoader loader(db_path);
        loader.print_stats();
        
        auto quotes = loader.load_all_quotes(product_id);
        
        if (quotes.empty()) {
            std::cerr << "ERROR: No quotes found for " << product_id << "\n";
            std::cerr << "Try running with different product or check database.\n";
            return 1;
        }
        
        std::cout << "Loaded " << quotes.size() << " REAL historical quotes\n";
        std::cout << "Time range: " << quotes.front().timestamp_us << " to " << quotes.back().timestamp_us << "\n";
        std::cout << "Price range: $" << quotes.front().best_bid << " to $" << quotes.back().best_bid << "\n\n";
        
        // Create backtester
        SQLiteBacktester backtester(50000.0, 0.1, 10.0);  // $50k capital, 0.1 BTC position, 10 bps fee
        
        // Test multiple strategies
        std::vector<std::unique_ptr<BacktestStrategy>> strategies;
        strategies.push_back(std::make_unique<ImbalanceStrategy>(0.2));
        strategies.push_back(std::make_unique<ImbalanceStrategy>(0.3));
        strategies.push_back(std::make_unique<ImbalanceStrategy>(0.4));
        strategies.push_back(std::make_unique<MeanReversionStrategy>(20, 1.5));
        strategies.push_back(std::make_unique<MeanReversionStrategy>(50, 2.0));
        strategies.push_back(std::make_unique<MomentumStrategy>(10, 0.0005));
        strategies.push_back(std::make_unique<MomentumStrategy>(20, 0.001));
        
        std::cout << "\n═══════════════════════════════════════════════════════════════\n";
        std::cout << "                    RUNNING BACKTESTS                           \n";
        std::cout << "═══════════════════════════════════════════════════════════════\n";
        
        for (auto& strategy : strategies) {
            auto result = backtester.run(quotes, *strategy);
            print_result(result);
        }
        
        std::cout << "\n✓ Backtest complete using REAL historical data from SQLite\n";
        
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
