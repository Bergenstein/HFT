#pragma once

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cmath>
#include <numeric>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <random>
#include <sqlite3.h>

namespace bt {

//==============================================================================
// FUNDING RATE DATA STRUCTURES
//==============================================================================

struct FundingRateSnapshot {
    int64_t timestamp_ms;           // Unix timestamp in milliseconds
    std::string exchange;
    std::string symbol;
    double funding_rate;            // 8-hour funding rate (e.g., 0.0001 = 0.01%)
    double mark_price;              // Mark price at funding time
    double index_price;             // Index/spot price
    double open_interest;           // Open interest in contracts
};

struct FundingArbPosition {
    std::string symbol;
    std::string long_exchange;      // Exchange where we're long
    std::string short_exchange;     // Exchange where we're short
    double entry_timestamp;
    double position_size_usd;       // Notional in USD
    double entry_long_funding;      // Funding rate when entered (long side)
    double entry_short_funding;     // Funding rate when entered (short side)
    double total_funding_pnl;       // Cumulative funding P&L
    double realized_pnl;            // Realized P&L on close
    int funding_periods_held;       // Number of 8-hour periods held
    bool is_open;
};

struct FundingArbTrade {
    int64_t timestamp;
    std::string action;             // "OPEN" or "CLOSE"
    std::string symbol;
    std::string long_exchange;
    std::string short_exchange;
    double position_size;
    double funding_diff_bps;        // Funding differential in bps
    double funding_pnl;             // P&L from this funding period
    double cumulative_pnl;          // Running total P&L
};

//==============================================================================
// FUNDING RATE BACKTEST RESULTS
//==============================================================================

struct FundingBacktestMetrics {
    // Core P&L
    double total_pnl = 0.0;
    double total_funding_collected = 0.0;
    double total_fees_paid = 0.0;
    double max_drawdown = 0.0;
    double peak_equity = 0.0;
    
    // Trade statistics
    int total_trades = 0;
    int winning_trades = 0;
    int losing_trades = 0;
    double win_rate = 0.0;
    double avg_trade_pnl = 0.0;
    double avg_winner = 0.0;
    double avg_loser = 0.0;
    double profit_factor = 0.0;
    
    // Funding-specific
    int total_funding_periods = 0;
    double avg_funding_diff_bps = 0.0;
    double max_funding_diff_bps = 0.0;
    double min_funding_diff_bps = 0.0;
    
    // Risk metrics (NOT annualized - as requested)
    double sharpe_ratio = 0.0;      // Daily Sharpe (not annualized)
    double sortino_ratio = 0.0;     // Daily Sortino (not annualized)
    double calmar_ratio = 0.0;      // Return / Max DD (not annualized)
    double volatility = 0.0;        // Daily volatility
    double downside_deviation = 0.0;
    
    // Time metrics
    double avg_holding_periods = 0.0;  // Avg number of 8-hour periods held
    double total_time_in_market_hours = 0.0;
    
    // Returns series for analysis
    std::vector<double> daily_returns;
    std::vector<double> equity_curve;
};

//==============================================================================
// FUNDING RATE BACKTESTER
//==============================================================================

class FundingRateBacktester {
public:
    struct Config {
        double initial_capital;
        double position_size_pct;
        double min_funding_diff_bps;
        double exit_funding_diff_bps;
        double max_holding_periods;
        double fee_rate_bps;
        bool enable_mean_reversion;
        double mean_reversion_zscore;
        
        Config() 
            : initial_capital(100000.0)
            , position_size_pct(0.5)
            , min_funding_diff_bps(5.0)
            , exit_funding_diff_bps(2.0)
            , max_holding_periods(12)
            , fee_rate_bps(4.0)
            , enable_mean_reversion(true)
            , mean_reversion_zscore(2.5)
        {}
    };
    
    FundingRateBacktester(const Config& config = Config{})
        : config_(config), equity_(config.initial_capital) {}
    
    //--------------------------------------------------------------------------
    // Load funding rate data from SQLite
    //--------------------------------------------------------------------------
    bool load_from_sqlite(const std::string& db_path) {
        sqlite3* db;
        if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
            std::cerr << "[BACKTEST] Failed to open database: " << db_path << "\n";
            return false;
        }
        
        const char* sql = R"(
            SELECT timestamp, exchange, symbol, funding_rate, mark_price, index_price, open_interest
            FROM funding_rates
            ORDER BY timestamp ASC
        )";
        
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[BACKTEST] Failed to prepare statement\n";
            sqlite3_close(db);
            return false;
        }
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FundingRateSnapshot snap;
            snap.timestamp_ms = sqlite3_column_int64(stmt, 0);
            snap.exchange = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            snap.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            snap.funding_rate = sqlite3_column_double(stmt, 3);
            snap.mark_price = sqlite3_column_double(stmt, 4);
            snap.index_price = sqlite3_column_double(stmt, 5);
            snap.open_interest = sqlite3_column_double(stmt, 6);
            funding_data_.push_back(snap);
        }
        
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        
        std::cout << "[BACKTEST] Loaded " << funding_data_.size() << " funding rate snapshots\n";
        return !funding_data_.empty();
    }
    
    //--------------------------------------------------------------------------
    // Load from simulated/generated funding data
    //--------------------------------------------------------------------------
    void load_simulated_data(const std::vector<FundingRateSnapshot>& data) {
        funding_data_ = data;
        std::cout << "[BACKTEST] Loaded " << funding_data_.size() << " simulated funding snapshots\n";
    }
    
    //--------------------------------------------------------------------------
    // Generate realistic funding rate test data
    //--------------------------------------------------------------------------
    static std::vector<FundingRateSnapshot> generate_test_data(int days = 30) {
        std::vector<FundingRateSnapshot> data;
        
        // Generate data for multiple exchanges
        std::vector<std::string> exchanges = {"binance", "bybit", "okx", "dydx"};
        std::vector<std::string> symbols = {"BTC-USDT-PERP", "ETH-USDT-PERP", "SOL-USDT-PERP"};
        
        // Base prices
        std::map<std::string, double> base_prices = {
            {"BTC-USDT-PERP", 97000.0},
            {"ETH-USDT-PERP", 3400.0},
            {"SOL-USDT-PERP", 240.0}
        };
        
        // Funding rate parameters per exchange (different characteristics)
        // Format: {mean, volatility, autocorrelation}
        std::map<std::string, std::tuple<double, double, double>> exchange_params = {
            {"binance", {0.0001, 0.0003, 0.7}},   // 0.01% mean, moderate vol
            {"bybit",   {0.00008, 0.00025, 0.6}}, // 0.008% mean, lower vol
            {"okx",     {0.00012, 0.00035, 0.65}},// 0.012% mean, higher vol
            {"dydx",    {0.00005, 0.0002, 0.5}}   // 0.005% mean, low vol (hourly funding)
        };
        
        int64_t start_ts = 1701388800000; // Dec 1, 2024
        int funding_periods = days * 3;    // 3 funding periods per day (8 hours each)
        
        std::mt19937 rng(42); // Deterministic seed for reproducibility
        std::normal_distribution<double> noise(0.0, 1.0);
        
        // Track previous funding rates for autocorrelation
        std::map<std::string, double> prev_rates;
        
        for (int i = 0; i < funding_periods; ++i) {
            int64_t ts = start_ts + i * 8 * 3600 * 1000; // 8 hours in ms
            
            for (const auto& symbol : symbols) {
                for (const auto& exchange : exchanges) {
                    std::string key = exchange + ":" + symbol;
                    
                    auto [mean, vol, autocorr] = exchange_params[exchange];
                    
                    // Generate autocorrelated funding rate
                    double prev = prev_rates.count(key) ? prev_rates[key] : mean;
                    double innovation = noise(rng) * vol;
                    double funding = autocorr * prev + (1 - autocorr) * mean + innovation;
                    
                    // Add occasional extreme funding (market stress)
                    if (std::abs(noise(rng)) > 2.5) {
                        funding += (noise(rng) > 0 ? 1 : -1) * vol * 3;
                    }
                    
                    // Clamp to realistic range [-0.75%, +0.75%]
                    funding = std::clamp(funding, -0.0075, 0.0075);
                    prev_rates[key] = funding;
                    
                    // Price with some noise
                    double price = base_prices[symbol] * (1.0 + noise(rng) * 0.02);
                    
                    FundingRateSnapshot snap;
                    snap.timestamp_ms = ts;
                    snap.exchange = exchange;
                    snap.symbol = symbol;
                    snap.funding_rate = funding;
                    snap.mark_price = price;
                    snap.index_price = price * (1.0 - funding * 0.5); // Simplified
                    snap.open_interest = 1000000000 + noise(rng) * 100000000;
                    
                    data.push_back(snap);
                }
            }
        }
        
        // Sort by timestamp
        std::sort(data.begin(), data.end(), 
            [](const auto& a, const auto& b) { return a.timestamp_ms < b.timestamp_ms; });
        
        return data;
    }
    
    //--------------------------------------------------------------------------
    // Run the backtest
    //--------------------------------------------------------------------------
    FundingBacktestMetrics run() {
        if (funding_data_.empty()) {
            std::cerr << "[BACKTEST] No data loaded!\n";
            return metrics_;
        }
        
        std::cout << "\n╔══════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║         FUNDING RATE ARBITRAGE BACKTEST                          ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ Initial Capital: $" << std::fixed << std::setprecision(0) 
                  << config_.initial_capital << std::setw(44) << "║\n";
        std::cout << "║ Min Funding Diff: " << config_.min_funding_diff_bps << " bps" 
                  << std::setw(44) << "║\n";
        std::cout << "║ Fee Rate: " << config_.fee_rate_bps << " bps per leg" 
                  << std::setw(44) << "║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";
        
        // Reset state
        equity_ = config_.initial_capital;
        positions_.clear();
        trades_.clear();
        metrics_ = FundingBacktestMetrics();
        metrics_.equity_curve.push_back(equity_);
        metrics_.peak_equity = equity_;
        
        // Group data by timestamp for processing
        int64_t prev_ts = 0;
        std::map<std::string, std::map<std::string, FundingRateSnapshot>> current_rates;
        
        double prev_equity = equity_;
        int64_t prev_day = 0;
        
        for (const auto& snap : funding_data_) {
            // New funding period
            if (snap.timestamp_ms != prev_ts && prev_ts != 0) {
                // Process funding period
                process_funding_period(current_rates, prev_ts);
                
                // Track daily returns
                int64_t current_day = snap.timestamp_ms / (24 * 3600 * 1000);
                if (current_day != prev_day && prev_day != 0) {
                    double daily_return = (equity_ - prev_equity) / prev_equity;
                    metrics_.daily_returns.push_back(daily_return);
                    prev_equity = equity_;
                }
                prev_day = current_day;
                
                current_rates.clear();
            }
            
            // Store rate
            current_rates[snap.symbol][snap.exchange] = snap;
            prev_ts = snap.timestamp_ms;
        }
        
        // Process final period
        if (!current_rates.empty()) {
            process_funding_period(current_rates, prev_ts);
        }
        
        // Close any remaining positions
        close_all_positions(prev_ts);
        
        // Calculate final metrics
        calculate_metrics();
        
        return metrics_;
    }
    
    //--------------------------------------------------------------------------
    // Get trade history
    //--------------------------------------------------------------------------
    const std::vector<FundingArbTrade>& get_trades() const { return trades_; }
    
    //--------------------------------------------------------------------------
    // Export results to CSV
    //--------------------------------------------------------------------------
    void export_to_csv(const std::string& path) const {
        std::ofstream out(path);
        out << "timestamp,action,symbol,long_exchange,short_exchange,position_size,"
            << "funding_diff_bps,funding_pnl,cumulative_pnl\n";
        
        for (const auto& trade : trades_) {
            out << trade.timestamp << ","
                << trade.action << ","
                << trade.symbol << ","
                << trade.long_exchange << ","
                << trade.short_exchange << ","
                << std::fixed << std::setprecision(2) << trade.position_size << ","
                << std::setprecision(4) << trade.funding_diff_bps << ","
                << std::setprecision(2) << trade.funding_pnl << ","
                << trade.cumulative_pnl << "\n";
        }
        
        std::cout << "[BACKTEST] Exported " << trades_.size() << " trades to " << path << "\n";
    }
    
    //--------------------------------------------------------------------------
    // Print detailed results
    //--------------------------------------------------------------------------
    void print_results() const {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                     FUNDING RATE ARBITRAGE BACKTEST RESULTS                  ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        
        // P&L Summary
        std::cout << "║ " << std::left << std::setw(35) << "Total P&L:" 
                  << std::right << std::setw(15) << std::fixed << std::setprecision(2) 
                  << "$" << metrics_.total_pnl << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Total Funding Collected:" 
                  << std::right << std::setw(15) << "$" << metrics_.total_funding_collected 
                  << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Total Fees Paid:" 
                  << std::right << std::setw(15) << "$" << metrics_.total_fees_paid 
                  << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Max Drawdown:" 
                  << std::right << std::setw(15) << std::setprecision(2) 
                  << (metrics_.max_drawdown * 100) << "%" << std::setw(24) << " ║\n";
        
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        
        // Trade Statistics
        std::cout << "║ " << std::left << std::setw(35) << "Total Trades:" 
                  << std::right << std::setw(15) << metrics_.total_trades 
                  << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Winning Trades:" 
                  << std::right << std::setw(15) << metrics_.winning_trades 
                  << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Losing Trades:" 
                  << std::right << std::setw(15) << metrics_.losing_trades 
                  << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Win Rate:" 
                  << std::right << std::setw(15) << std::setprecision(1) 
                  << (metrics_.win_rate * 100) << "%" << std::setw(24) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Profit Factor:" 
                  << std::right << std::setw(15) << std::setprecision(2) 
                  << metrics_.profit_factor << std::setw(25) << " ║\n";
        
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        
        // Funding Statistics
        std::cout << "║ " << std::left << std::setw(35) << "Total Funding Periods:" 
                  << std::right << std::setw(15) << metrics_.total_funding_periods 
                  << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Avg Funding Diff (bps):" 
                  << std::right << std::setw(15) << std::setprecision(2) 
                  << metrics_.avg_funding_diff_bps << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Max Funding Diff (bps):" 
                  << std::right << std::setw(15) << metrics_.max_funding_diff_bps 
                  << std::setw(25) << " ║\n";
        
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        
        // Risk Metrics (NOT ANNUALIZED)
        std::cout << "║ RISK METRICS (Daily, NOT Annualized)                                         ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ " << std::left << std::setw(35) << "Daily Sharpe Ratio:" 
                  << std::right << std::setw(15) << std::setprecision(3) 
                  << metrics_.sharpe_ratio << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Daily Sortino Ratio:" 
                  << std::right << std::setw(15) << metrics_.sortino_ratio 
                  << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Calmar Ratio:" 
                  << std::right << std::setw(15) << metrics_.calmar_ratio 
                  << std::setw(25) << " ║\n";
        std::cout << "║ " << std::left << std::setw(35) << "Daily Volatility:" 
                  << std::right << std::setw(15) << std::setprecision(4) 
                  << (metrics_.volatility * 100) << "%" << std::setw(24) << " ║\n";
        
        std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    }
    
private:
    Config config_;
    double equity_;
    std::vector<FundingRateSnapshot> funding_data_;
    std::map<std::string, FundingArbPosition> positions_;  // symbol -> position
    std::vector<FundingArbTrade> trades_;
    FundingBacktestMetrics metrics_;
    std::deque<double> funding_diff_history_;
    
    //--------------------------------------------------------------------------
    // Process one funding period
    //--------------------------------------------------------------------------
    void process_funding_period(
        const std::map<std::string, std::map<std::string, FundingRateSnapshot>>& rates,
        int64_t timestamp) {
        
        metrics_.total_funding_periods++;
        
        // 1. Collect funding for existing positions
        for (auto& [symbol, pos] : positions_) {
            if (!pos.is_open) continue;
            
            auto it = rates.find(symbol);
            if (it == rates.end()) continue;
            
            const auto& symbol_rates = it->second;
            
            auto long_it = symbol_rates.find(pos.long_exchange);
            auto short_it = symbol_rates.find(pos.short_exchange);
            
            if (long_it == symbol_rates.end() || short_it == symbol_rates.end()) continue;
            
            double long_funding = long_it->second.funding_rate;
            double short_funding = short_it->second.funding_rate;
            
            // Funding P&L:
            // - Long position: We PAY funding if positive, RECEIVE if negative
            // - Short position: We RECEIVE funding if positive, PAY if negative
            // Net = short_funding - long_funding (we want short_funding > long_funding)
            double funding_diff = short_funding - long_funding;
            double funding_pnl = funding_diff * pos.position_size_usd;
            
            pos.total_funding_pnl += funding_pnl;
            pos.funding_periods_held++;
            
            equity_ += funding_pnl;
            metrics_.total_funding_collected += funding_pnl;
            
            // Track for metrics
            double diff_bps = funding_diff * 10000.0;
            funding_diff_history_.push_back(diff_bps);
            if (funding_diff_history_.size() > 1000) {
                funding_diff_history_.pop_front();
            }
            
            // Record trade
            FundingArbTrade trade;
            trade.timestamp = timestamp;
            trade.action = "FUNDING";
            trade.symbol = symbol;
            trade.long_exchange = pos.long_exchange;
            trade.short_exchange = pos.short_exchange;
            trade.position_size = pos.position_size_usd;
            trade.funding_diff_bps = diff_bps;
            trade.funding_pnl = funding_pnl;
            trade.cumulative_pnl = equity_ - config_.initial_capital;
            trades_.push_back(trade);
            
            // Check exit conditions
            if (std::abs(diff_bps) < config_.exit_funding_diff_bps ||
                pos.funding_periods_held >= config_.max_holding_periods) {
                close_position(symbol, timestamp, "EXIT_SIGNAL");
            }
        }
        
        // 2. Look for new opportunities
        for (const auto& [symbol, symbol_rates] : rates) {
            // Skip if already have position
            if (positions_.count(symbol) && positions_[symbol].is_open) continue;
            
            // Find best funding differential
            std::string best_long, best_short;
            double max_diff = 0.0;
            double best_long_rate = 0.0, best_short_rate = 0.0;
            
            for (const auto& [ex1, snap1] : symbol_rates) {
                for (const auto& [ex2, snap2] : symbol_rates) {
                    if (ex1 == ex2) continue;
                    
                    // Try long ex1, short ex2
                    double diff = snap2.funding_rate - snap1.funding_rate;
                    if (diff > max_diff) {
                        max_diff = diff;
                        best_long = ex1;
                        best_short = ex2;
                        best_long_rate = snap1.funding_rate;
                        best_short_rate = snap2.funding_rate;
                    }
                }
            }
            
            double diff_bps = max_diff * 10000.0;
            
            // Check entry threshold
            if (diff_bps >= config_.min_funding_diff_bps) {
                open_position(symbol, best_long, best_short, 
                             best_long_rate, best_short_rate, timestamp);
            }
        }
        
        // Update equity curve
        metrics_.equity_curve.push_back(equity_);
        
        // Track drawdown
        if (equity_ > metrics_.peak_equity) {
            metrics_.peak_equity = equity_;
        }
        double dd = (metrics_.peak_equity - equity_) / metrics_.peak_equity;
        if (dd > metrics_.max_drawdown) {
            metrics_.max_drawdown = dd;
        }
    }
    
    //--------------------------------------------------------------------------
    // Open a new position
    //--------------------------------------------------------------------------
    void open_position(const std::string& symbol, 
                       const std::string& long_ex,
                       const std::string& short_ex,
                       double long_funding,
                       double short_funding,
                       int64_t timestamp) {
        
        double position_size = equity_ * config_.position_size_pct;
        
        // Pay fees for both legs (entry)
        double fees = position_size * 2 * (config_.fee_rate_bps / 10000.0);
        equity_ -= fees;
        metrics_.total_fees_paid += fees;
        
        FundingArbPosition pos;
        pos.symbol = symbol;
        pos.long_exchange = long_ex;
        pos.short_exchange = short_ex;
        pos.entry_timestamp = timestamp;
        pos.position_size_usd = position_size;
        pos.entry_long_funding = long_funding;
        pos.entry_short_funding = short_funding;
        pos.total_funding_pnl = 0.0;
        pos.realized_pnl = 0.0;
        pos.funding_periods_held = 0;
        pos.is_open = true;
        
        positions_[symbol] = pos;
        
        double diff_bps = (short_funding - long_funding) * 10000.0;
        
        FundingArbTrade trade;
        trade.timestamp = timestamp;
        trade.action = "OPEN";
        trade.symbol = symbol;
        trade.long_exchange = long_ex;
        trade.short_exchange = short_ex;
        trade.position_size = position_size;
        trade.funding_diff_bps = diff_bps;
        trade.funding_pnl = -fees; // Entry cost
        trade.cumulative_pnl = equity_ - config_.initial_capital;
        trades_.push_back(trade);
        
        std::cout << "[OPEN] " << symbol << " | Long " << long_ex << ", Short " << short_ex
                  << " | Diff: " << std::fixed << std::setprecision(2) << diff_bps << " bps"
                  << " | Size: $" << std::setprecision(0) << position_size << "\n";
    }
    
    //--------------------------------------------------------------------------
    // Close a position
    //--------------------------------------------------------------------------
    void close_position(const std::string& symbol, int64_t timestamp, const std::string& reason) {
        auto it = positions_.find(symbol);
        if (it == positions_.end() || !it->second.is_open) return;
        
        auto& pos = it->second;
        
        // Pay fees for both legs (exit)
        double fees = pos.position_size_usd * 2 * (config_.fee_rate_bps / 10000.0);
        equity_ -= fees;
        metrics_.total_fees_paid += fees;
        
        pos.realized_pnl = pos.total_funding_pnl - fees;
        pos.is_open = false;
        
        // Track trade statistics
        metrics_.total_trades++;
        if (pos.total_funding_pnl > 0) {
            metrics_.winning_trades++;
        } else {
            metrics_.losing_trades++;
        }
        
        FundingArbTrade trade;
        trade.timestamp = timestamp;
        trade.action = "CLOSE";
        trade.symbol = symbol;
        trade.long_exchange = pos.long_exchange;
        trade.short_exchange = pos.short_exchange;
        trade.position_size = pos.position_size_usd;
        trade.funding_diff_bps = 0.0;
        trade.funding_pnl = pos.total_funding_pnl;
        trade.cumulative_pnl = equity_ - config_.initial_capital;
        trades_.push_back(trade);
        
        std::cout << "[CLOSE] " << symbol << " | " << reason
                  << " | Periods: " << pos.funding_periods_held
                  << " | Funding P&L: $" << std::fixed << std::setprecision(2) << pos.total_funding_pnl
                  << " | Net P&L: $" << pos.realized_pnl << "\n";
    }
    
    //--------------------------------------------------------------------------
    // Close all positions at end of backtest
    //--------------------------------------------------------------------------
    void close_all_positions(int64_t timestamp) {
        for (auto& [symbol, pos] : positions_) {
            if (pos.is_open) {
                close_position(symbol, timestamp, "END_OF_BACKTEST");
            }
        }
    }
    
    //--------------------------------------------------------------------------
    // Calculate final metrics
    //--------------------------------------------------------------------------
    void calculate_metrics() {
        metrics_.total_pnl = equity_ - config_.initial_capital;
        
        // Win rate and averages
        if (metrics_.total_trades > 0) {
            metrics_.win_rate = static_cast<double>(metrics_.winning_trades) / metrics_.total_trades;
            
            double total_winners = 0, total_losers = 0;
            for (const auto& [symbol, pos] : positions_) {
                if (pos.total_funding_pnl > 0) {
                    total_winners += pos.total_funding_pnl;
                } else {
                    total_losers += std::abs(pos.total_funding_pnl);
                }
            }
            
            if (metrics_.winning_trades > 0) {
                metrics_.avg_winner = total_winners / metrics_.winning_trades;
            }
            if (metrics_.losing_trades > 0) {
                metrics_.avg_loser = total_losers / metrics_.losing_trades;
            }
            if (total_losers > 0) {
                metrics_.profit_factor = total_winners / total_losers;
            }
            metrics_.avg_trade_pnl = metrics_.total_pnl / metrics_.total_trades;
        }
        
        // Funding diff statistics
        if (!funding_diff_history_.empty()) {
            double sum = std::accumulate(funding_diff_history_.begin(), 
                                         funding_diff_history_.end(), 0.0);
            metrics_.avg_funding_diff_bps = sum / funding_diff_history_.size();
            metrics_.max_funding_diff_bps = *std::max_element(
                funding_diff_history_.begin(), funding_diff_history_.end());
            metrics_.min_funding_diff_bps = *std::min_element(
                funding_diff_history_.begin(), funding_diff_history_.end());
        }
        
        // Risk metrics (NOT ANNUALIZED)
        if (metrics_.daily_returns.size() > 1) {
            // Mean daily return
            double mean = std::accumulate(metrics_.daily_returns.begin(),
                                         metrics_.daily_returns.end(), 0.0) 
                         / metrics_.daily_returns.size();
            
            // Volatility (daily)
            double sq_sum = 0.0;
            for (double r : metrics_.daily_returns) {
                sq_sum += (r - mean) * (r - mean);
            }
            metrics_.volatility = std::sqrt(sq_sum / metrics_.daily_returns.size());
            
            // Downside deviation
            double neg_sq_sum = 0.0;
            int neg_count = 0;
            for (double r : metrics_.daily_returns) {
                if (r < 0) {
                    neg_sq_sum += r * r;
                    neg_count++;
                }
            }
            if (neg_count > 0) {
                metrics_.downside_deviation = std::sqrt(neg_sq_sum / neg_count);
            }
            
            // Sharpe (daily, NOT annualized)
            if (metrics_.volatility > 0) {
                metrics_.sharpe_ratio = mean / metrics_.volatility;
            }
            
            // Sortino (daily, NOT annualized)
            if (metrics_.downside_deviation > 0) {
                metrics_.sortino_ratio = mean / metrics_.downside_deviation;
            }
            
            // Calmar
            if (metrics_.max_drawdown > 0) {
                double total_return = metrics_.total_pnl / config_.initial_capital;
                metrics_.calmar_ratio = total_return / metrics_.max_drawdown;
            }
        }
        
        // Average holding periods
        double total_periods = 0;
        int count = 0;
        for (const auto& [symbol, pos] : positions_) {
            if (pos.funding_periods_held > 0) {
                total_periods += pos.funding_periods_held;
                count++;
            }
        }
        if (count > 0) {
            metrics_.avg_holding_periods = total_periods / count;
        }
        
        metrics_.total_time_in_market_hours = total_periods * 8.0;
    }
};

} // namespace bt
