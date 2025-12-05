//==============================================================================
// BACKTEST TO JSON
//==============================================================================
// Runs funding rate arbitrage backtest and outputs JSON file.
//
// Usage:
//   ./build/backtest_to_json BTCUSDT 1.0
//   ./build/backtest_to_json ETHUSDT 2.0 50000 output.json
//==============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <cmath>
#include <iomanip>

#include "../arb/historical_funding_fetcher.hpp"
#include "../arb/real_funding_rate_fetcher.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main(int argc, char* argv[]) {
    std::string symbol = "BTCUSDT";
    double min_diff_bps = 1.0;
    double position_size = 100000.0;
    std::string output_file = "backtest_results.json";
    
    if (argc >= 2) symbol = argv[1];
    if (argc >= 3) min_diff_bps = std::stod(argv[2]);
    if (argc >= 4) position_size = std::stod(argv[3]);
    if (argc >= 5) output_file = argv[4];
    
    // Load REAL historical funding rates
    std::string cache_file = "data/funding_history_" + symbol + ".csv";
    std::vector<arb::HistoricalFundingRate> rates;
    
    std::ifstream test_file(cache_file);
    if (test_file.good()) {
        rates = arb::HistoricalFundingFetcher::load_from_csv(cache_file);
    } else {
        rates = arb::HistoricalFundingFetcher::fetch_all_history(symbol);
        if (!rates.empty()) {
            arb::HistoricalFundingFetcher::save_to_csv(rates, cache_file);
        }
    }
    
    if (rates.empty()) {
        std::cerr << "ERROR: No historical data\n";
        return 1;
    }
    
    // Group rates by funding period
    std::map<int64_t, std::vector<arb::HistoricalFundingRate>> by_period;
    for (const auto& r : rates) {
        int64_t period = (r.funding_time_ms / (8 * 3600 * 1000)) * (8 * 3600 * 1000);
        by_period[period].push_back(r);
    }
    
    // Run backtest - collect ALL data points
    json result;
    result["symbol"] = symbol;
    result["min_diff_bps"] = min_diff_bps;
    result["position_size"] = position_size;
    result["data_source"] = "REAL exchange APIs (Binance, Bybit, OKX)";
    
    std::vector<json> equity_curve;
    std::vector<json> trades;
    std::vector<double> daily_returns;
    
    double equity = position_size;
    double peak_equity = equity;
    double max_drawdown = 0;
    double total_pnl = 0;
    double total_fees = 0;
    int total_trades = 0;
    int winning_trades = 0;
    double day_pnl = 0;
    int64_t last_day = 0;
    
    for (const auto& [period_time, period_rates] : by_period) {
        if (period_rates.size() < 2) continue;
        
        // Find best arbitrage opportunity
        double max_rate = period_rates[0].funding_rate;
        double min_rate = period_rates[0].funding_rate;
        std::string max_ex = period_rates[0].exchange;
        std::string min_ex = period_rates[0].exchange;
        
        for (const auto& r : period_rates) {
            if (r.funding_rate > max_rate) { max_rate = r.funding_rate; max_ex = r.exchange; }
            if (r.funding_rate < min_rate) { min_rate = r.funding_rate; min_ex = r.exchange; }
        }
        
        double diff = max_rate - min_rate;
        double diff_bps = diff * 10000;
        
        double period_pnl = 0;
        double period_fees = 0;
        
        if (diff_bps >= min_diff_bps) {
            auto fees_long = arb::ExchangeFees::get(min_ex);
            auto fees_short = arb::ExchangeFees::get(max_ex);
            
            double funding_pnl = diff * position_size;
            double entry_fee = position_size * (fees_long.maker_fee + fees_short.taker_fee);
            double exit_fee = entry_fee;
            
            period_pnl = funding_pnl - entry_fee - exit_fee;
            period_fees = entry_fee + exit_fee;
            
            total_trades++;
            if (period_pnl > 0) winning_trades++;
            
            // Record trade
            trades.push_back({
                {"timestamp_ms", period_time},
                {"long_exchange", min_ex},
                {"short_exchange", max_ex},
                {"long_rate", min_rate * 100},
                {"short_rate", max_rate * 100},
                {"diff_bps", diff_bps},
                {"funding_pnl", funding_pnl},
                {"fees", period_fees},
                {"net_pnl", period_pnl}
            });
        }
        
        total_pnl += period_pnl;
        total_fees += period_fees;
        equity += period_pnl;
        
        if (equity > peak_equity) peak_equity = equity;
        double drawdown = (peak_equity - equity) / peak_equity;
        if (drawdown > max_drawdown) max_drawdown = drawdown;
        
        // Record equity point
        equity_curve.push_back({
            {"timestamp_ms", period_time},
            {"equity", equity},
            {"drawdown", drawdown}
        });
        
        // Track daily returns
        int64_t day = period_time / (24 * 3600 * 1000);
        if (day != last_day && last_day != 0) {
            daily_returns.push_back(day_pnl / position_size);
            day_pnl = 0;
        }
        day_pnl += period_pnl;
        last_day = day;
    }
    
    if (day_pnl != 0) daily_returns.push_back(day_pnl / position_size);
    
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
    
    double daily_sharpe = variance > 0 ? mean_return / std::sqrt(variance) : 0;
    double daily_sortino = downside_var > 0 ? mean_return / std::sqrt(downside_var) : 0;
    
    // Build final result
    result["summary"] = {
        {"total_periods", by_period.size()},
        {"total_trades", total_trades},
        {"winning_trades", winning_trades},
        {"losing_trades", total_trades - winning_trades},
        {"win_rate", total_trades > 0 ? (double)winning_trades / total_trades : 0},
        {"initial_equity", position_size},
        {"final_equity", equity},
        {"total_pnl", total_pnl},
        {"total_fees", total_fees},
        {"net_return_pct", total_pnl / position_size * 100},
        {"max_drawdown_pct", max_drawdown * 100},
        {"daily_sharpe", daily_sharpe},
        {"daily_sortino", daily_sortino},
        {"daily_volatility", std::sqrt(variance)},
        {"avg_trade_pnl", total_trades > 0 ? total_pnl / total_trades : 0}
    };
    
    result["equity_curve"] = equity_curve;
    result["trades"] = trades;
    
    // Write to file
    std::ofstream out(output_file);
    out << result.dump(2);
    out.close();
    
    return 0;
}
