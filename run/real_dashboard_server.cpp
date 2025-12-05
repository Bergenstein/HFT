//==============================================================================
// REAL DASHBOARD SERVER - NO SIMULATION
//==============================================================================
// Serves REAL backtest metrics and live strategy performance via ZMQ.
// This runs on the cold path (non-latency critical).
//
// Data Sources (ALL REAL):
//   1. Historical funding rate backtest results (from CSV/SQLite)
//   2. Live funding rate scanner results
//   3. Real trade execution logs
//
// Architecture:
//   [Real Backtest] -> [ZMQ Publisher] -> [Dashboard Subscriber] -> [Browser]
//
// Usage:
//   ./build/real_dashboard_server --backtest BTCUSDT    # Show backtest results
//   ./build/real_dashboard_server --live                 # Show live scanner
//
//==============================================================================

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <map>

#include "../zmq/strategy_metrics_publisher.hpp"
#include "../arb/historical_funding_fetcher.hpp"
#include "../arb/real_funding_rate_fetcher.hpp"

std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\n[SIGNAL] Caught signal " << sig << ", shutting down...\n";
    g_running.store(false);
}

//==============================================================================
// REAL BACKTEST METRICS PUBLISHER
//==============================================================================

class RealBacktestMetricsPublisher {
public:
    RealBacktestMetricsPublisher(const std::string& zmq_endpoint = "tcp://*:5555")
        : publisher_(zmq_endpoint) {
        std::cout << "[ZMQ] Publishing metrics on " << zmq_endpoint << "\n";
    }
    
    // Run backtest with REAL historical data and publish metrics
    void run_and_publish_backtest(
        const std::string& symbol,
        double position_size = 100000.0,
        double min_diff_bps = 5.0
    ) {
        std::cout << "\n[BACKTEST] Running with REAL historical data...\n";
        std::cout << "[BACKTEST] Symbol: " << symbol << "\n";
        std::cout << "[BACKTEST] Position: $" << position_size << "\n";
        std::cout << "[BACKTEST] Min Spread: " << min_diff_bps << " bps\n\n";
        
        // Load REAL historical funding rates
        std::string cache_file = "data/funding_history_" + symbol + ".csv";
        std::vector<arb::HistoricalFundingRate> rates;
        
        std::ifstream test_file(cache_file);
        if (test_file.good()) {
            std::cout << "[DATA] Loading cached REAL data from " << cache_file << "\n";
            rates = arb::HistoricalFundingFetcher::load_from_csv(cache_file);
        } else {
            std::cout << "[DATA] Fetching REAL historical data from exchanges...\n";
            rates = arb::HistoricalFundingFetcher::fetch_all_history(symbol);
            if (!rates.empty()) {
                arb::HistoricalFundingFetcher::save_to_csv(rates, cache_file);
            }
        }
        
        if (rates.empty()) {
            std::cerr << "[ERROR] No historical data available!\n";
            return;
        }
        
        std::cout << "[DATA] Loaded " << rates.size() << " REAL funding rate records\n\n";
        
        // Group rates by funding period
        std::map<int64_t, std::vector<arb::HistoricalFundingRate>> by_period;
        for (const auto& r : rates) {
            int64_t period = (r.funding_time_ms / (8 * 3600 * 1000)) * (8 * 3600 * 1000);
            by_period[period].push_back(r);
        }
        
        // Run backtest and publish metrics for each period
        double equity = position_size;
        double peak_equity = equity;
        double total_pnl = 0.0;
        double total_fees = 0.0;
        int total_trades = 0;
        int winning_trades = 0;
        std::vector<double> daily_returns;
        double day_pnl = 0.0;
        int64_t last_day = 0;
        
        std::cout << "[BACKTEST] Processing " << by_period.size() << " funding periods...\n";
        std::cout << "[ZMQ] Publishing metrics to dashboard...\n\n";
        
        int period_count = 0;
        for (const auto& [period_time, period_rates] : by_period) {
            if (!g_running.load()) break;
            
            if (period_rates.size() < 2) continue;
            
            // Find best arbitrage opportunity
            double max_rate = period_rates[0].funding_rate;
            double min_rate = period_rates[0].funding_rate;
            std::string max_ex = period_rates[0].exchange;
            std::string min_ex = period_rates[0].exchange;
            
            for (const auto& r : period_rates) {
                if (r.funding_rate > max_rate) {
                    max_rate = r.funding_rate;
                    max_ex = r.exchange;
                }
                if (r.funding_rate < min_rate) {
                    min_rate = r.funding_rate;
                    min_ex = r.exchange;
                }
            }
            
            double diff = max_rate - min_rate;
            double diff_bps = diff * 10000;
            
            // Calculate trade P&L if spread meets threshold
            double period_pnl = 0.0;
            double period_fees = 0.0;
            bool took_trade = false;
            
            if (diff_bps >= min_diff_bps) {
                // REAL fees
                auto fees_long = arb::ExchangeFees::get(min_ex);
                auto fees_short = arb::ExchangeFees::get(max_ex);
                
                double funding_pnl = diff * position_size;
                double entry_fee = position_size * (fees_long.maker_fee + fees_short.taker_fee);
                double exit_fee = entry_fee;
                
                period_pnl = funding_pnl - entry_fee - exit_fee;
                period_fees = entry_fee + exit_fee;
                
                total_trades++;
                took_trade = true;
                if (period_pnl > 0) winning_trades++;
            }
            
            total_pnl += period_pnl;
            total_fees += period_fees;
            equity += period_pnl;
            
            if (equity > peak_equity) peak_equity = equity;
            double drawdown = (peak_equity - equity) / peak_equity;
            
            // Track daily returns
            int64_t day = period_time / (24 * 3600 * 1000);
            if (day != last_day && last_day != 0) {
                daily_returns.push_back(day_pnl / position_size);
                day_pnl = 0;
            }
            day_pnl += period_pnl;
            last_day = day;
            
            // Calculate metrics (NOT ANNUALIZED)
            double mean_return = 0, variance = 0, downside_var = 0;
            if (!daily_returns.empty()) {
                for (double r : daily_returns) mean_return += r;
                mean_return /= daily_returns.size();
                
                for (double r : daily_returns) {
                    variance += (r - mean_return) * (r - mean_return);
                    if (r < 0) downside_var += r * r;
                }
                variance /= daily_returns.size();
                downside_var /= daily_returns.size();
            }
            
            double std_dev = std::sqrt(variance);
            double downside_std = std::sqrt(downside_var);
            double daily_sharpe = std_dev > 0 ? mean_return / std_dev : 0;
            double daily_sortino = downside_std > 0 ? mean_return / downside_std : 0;
            
            // Publish metrics via ZMQ
            zmq_metrics::StrategyMetrics m;
            m.strategy_name = "FundingRateArb_REAL";
            m.symbol = symbol;
            m.timestamp_ms = period_time;
            
            m.total_pnl = total_pnl;
            m.unrealized_pnl = 0;
            m.realized_pnl = total_pnl;
            m.daily_pnl = period_pnl;
            
            m.position_size = took_trade ? position_size : 0;
            m.position_value = took_trade ? position_size : 0;
            m.avg_entry_price = 0;
            
            m.total_trades = total_trades;
            m.winning_trades = winning_trades;
            m.losing_trades = total_trades - winning_trades;
            m.win_rate = total_trades > 0 ? static_cast<double>(winning_trades) / total_trades : 0;
            m.avg_trade_pnl = total_trades > 0 ? total_pnl / total_trades : 0;
            
            // NOT ANNUALIZED metrics
            m.sharpe_ratio = daily_sharpe;
            m.sortino_ratio = daily_sortino;
            m.max_drawdown = drawdown;
            m.current_drawdown = drawdown;
            m.volatility = std_dev;
            m.calmar_ratio = drawdown > 0 ? (total_pnl / position_size) / drawdown : 0;
            
            m.funding_collected = total_pnl + total_fees;  // Gross funding before fees
            m.avg_funding_diff_bps = diff_bps;
            m.hours_in_position = total_trades * 8.0;
            m.funding_periods = period_count;
            
            m.latency_us = 0;  // Not applicable for backtest
            m.signals_generated = total_trades;
            m.orders_sent = total_trades * 2;
            m.fills_received = total_trades * 2;
            
            publisher_.publish_metrics(m);
            
            // Publish equity point
            zmq_metrics::EquityCurvePoint ep;
            ep.timestamp_ms = period_time;
            ep.equity = equity;
            ep.drawdown = drawdown;
            publisher_.publish_equity(ep);
            
            period_count++;
            
            // Print progress every 20 periods
            if (period_count % 20 == 0) {
                std::cout << "[PERIOD " << period_count << "] "
                          << "Equity: $" << std::fixed << std::setprecision(2) << equity
                          << " | P&L: $" << total_pnl
                          << " | Trades: " << total_trades
                          << " | Sharpe: " << std::setprecision(3) << daily_sharpe << "\n";
            }
            
            // Small delay to not overwhelm ZMQ subscribers
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        // Add last day
        if (day_pnl != 0) {
            daily_returns.push_back(day_pnl / position_size);
        }
        
        // Print final summary
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                    BACKTEST COMPLETE - REAL DATA                             ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "║ Symbol:            " << std::left << std::setw(15) << symbol << std::string(41, ' ') << "║\n";
        std::cout << "║ Total Periods:     " << std::setw(10) << period_count << std::string(46, ' ') << "║\n";
        std::cout << "║ Total Trades:      " << std::setw(10) << total_trades << std::string(46, ' ') << "║\n";
        std::cout << "║ Winning Trades:    " << std::setw(10) << winning_trades << std::string(46, ' ') << "║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ Final Equity:     $" << std::setw(15) << equity << std::string(41, ' ') << "║\n";
        std::cout << "║ Total P&L:        $" << std::setw(15) << total_pnl << std::string(41, ' ') << "║\n";
        std::cout << "║ Total Fees:       $" << std::setw(15) << total_fees << std::string(41, ' ') << "║\n";
        std::cout << "║ Net Return:        " << std::setw(10) << std::setprecision(2) << (total_pnl / position_size * 100) << "%" << std::string(44, ' ') << "║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    }
    
    // Run live scanner and publish metrics
    void run_live_scanner(const std::vector<std::string>& symbols) {
        std::cout << "\n[LIVE] Starting REAL funding rate scanner...\n";
        std::cout << "[LIVE] Symbols: ";
        for (const auto& s : symbols) std::cout << s << " ";
        std::cout << "\n\n";
        
        while (g_running.load()) {
            for (const auto& symbol : symbols) {
                if (!g_running.load()) break;
                
                auto rates = arb::RealFundingRateFetcher::fetch_all(symbol);
                
                if (rates.size() >= 2) {
                    // Find best opportunity
                    const arb::RealFundingRate* max_rate = &rates[0];
                    const arb::RealFundingRate* min_rate = &rates[0];
                    
                    for (const auto& r : rates) {
                        if (r.funding_rate > max_rate->funding_rate) max_rate = &r;
                        if (r.funding_rate < min_rate->funding_rate) min_rate = &r;
                    }
                    
                    double diff = max_rate->funding_rate - min_rate->funding_rate;
                    double diff_bps = diff * 10000;
                    
                    // Publish opportunity
                    zmq_metrics::StrategyMetrics m;
                    m.strategy_name = "FundingRateArb_LIVE";
                    m.symbol = symbol;
                    m.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    m.avg_funding_diff_bps = diff_bps;
                    m.funding_collected = diff * 100000;  // Per $100k position
                    
                    publisher_.publish_metrics(m);
                    
                    std::cout << "[" << symbol << "] "
                              << "Long " << min_rate->exchange << " (" << (min_rate->funding_rate * 100) << "%) "
                              << "Short " << max_rate->exchange << " (" << (max_rate->funding_rate * 100) << "%) "
                              << "Diff: " << std::fixed << std::setprecision(2) << diff_bps << " bps\n";
                }
            }
            
            std::cout << "\n[LIVE] Next scan in 60 seconds...\n\n";
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
    }
    
private:
    zmq_metrics::StrategyMetricsPublisher publisher_;
};

//==============================================================================
// MAIN
//==============================================================================

void print_usage() {
    std::cout << "Usage:\n";
    std::cout << "  ./real_dashboard_server --backtest BTCUSDT [min_spread_bps]\n";
    std::cout << "  ./real_dashboard_server --live BTCUSDT,ETHUSDT,SOLUSDT\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --backtest   Run historical backtest with REAL data\n";
    std::cout << "  --live       Run live funding rate scanner with REAL data\n";
    std::cout << "\nExample:\n";
    std::cout << "  ./real_dashboard_server --backtest BTCUSDT 3.0\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              REAL DASHBOARD SERVER - NO SIMULATION                           ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Data Source: REAL exchange APIs and historical data                          ║\n";
    std::cout << "║ ZMQ Endpoint: tcp://*:5555                                                   ║\n";
    std::cout << "║ Dashboard: Open dashboard/strategy_dashboard.html in browser                 ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    
    if (argc < 3) {
        print_usage();
        return 1;
    }
    
    std::string mode = argv[1];
    std::string symbols_str = argv[2];
    double min_spread = 3.0;
    
    if (argc >= 4) {
        min_spread = std::stod(argv[3]);
    }
    
    RealBacktestMetricsPublisher publisher;
    
    if (mode == "--backtest") {
        publisher.run_and_publish_backtest(symbols_str, 100000.0, min_spread);
    } else if (mode == "--live") {
        std::vector<std::string> symbols;
        std::istringstream iss(symbols_str);
        std::string symbol;
        while (std::getline(iss, symbol, ',')) {
            symbols.push_back(symbol);
        }
        publisher.run_live_scanner(symbols);
    } else {
        print_usage();
        return 1;
    }
    
    std::cout << "\n[DONE] Dashboard server finished.\n";
    return 0;
}
