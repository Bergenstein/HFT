//==============================================================================
// BACKTEST WITH REAL HISTORICAL FUNDING RATES - NO SIMULATION
//==============================================================================
// This fetches REAL historical funding rates from exchanges and backtests
// the funding rate arbitrage strategy using REAL data.
//
// Build: make build/backtest_real_funding
// Run:   ./build/backtest_real_funding BTCUSDT [min_diff_bps]
//==============================================================================

#include <iostream>
#include <iomanip>
#include <fstream>

#include "../arb/historical_funding_fetcher.hpp"

using namespace arb;

int main(int argc, char* argv[]) {
    std::string symbol = "BTCUSDT";
    double min_diff_bps = 5.0;  // Minimum spread to enter trade
    double position_size = 100000.0;
    bool save_data = true;
    
    if (argc >= 2) {
        symbol = argv[1];
    }
    if (argc >= 3) {
        min_diff_bps = std::stod(argv[2]);
    }
    if (argc >= 4) {
        position_size = std::stod(argv[3]);
    }
    
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║        FUNDING RATE ARBITRAGE BACKTEST - REAL HISTORICAL DATA               ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Symbol:          " << std::left << std::setw(15) << symbol << std::string(44, ' ') << "║\n";
    std::cout << "║ Min Spread:      " << std::setw(10) << min_diff_bps << " bps" << std::string(44, ' ') << "║\n";
    std::cout << "║ Position Size:  $" << std::setw(15) << std::fixed << std::setprecision(0) << position_size << std::string(43, ' ') << "║\n";
    std::cout << "║ Data Source:     REAL exchange APIs (NO SIMULATION)                          ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    
    // Check if we have cached data
    std::string cache_file = "data/funding_history_" + symbol + ".csv";
    std::vector<HistoricalFundingRate> rates;
    
    std::ifstream test_file(cache_file);
    if (test_file.good()) {
        std::cout << "\n[INFO] Found cached data at " << cache_file << "\n";
        std::cout << "[INFO] Loading cached data...\n";
        rates = HistoricalFundingFetcher::load_from_csv(cache_file);
    }
    
    if (rates.empty()) {
        std::cout << "\n[INFO] Fetching REAL historical funding rates from exchanges...\n";
        std::cout << "[INFO] This may take a moment...\n\n";
        
        // Fetch real historical data
        rates = HistoricalFundingFetcher::fetch_all_history(symbol);
        
        if (rates.empty()) {
            std::cerr << "[ERROR] Failed to fetch any historical funding data\n";
            return 1;
        }
        
        // Save to cache
        if (save_data) {
            std::cout << "\n[INFO] Saving data to cache...\n";
            HistoricalFundingFetcher::save_to_csv(rates, cache_file);
        }
    }
    
    // Show data summary
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                          DATA SUMMARY                                        ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    
    // Count by exchange
    int binance_count = 0, bybit_count = 0, okx_count = 0;
    int64_t min_time = INT64_MAX, max_time = 0;
    
    for (const auto& r : rates) {
        if (r.exchange == "binance") binance_count++;
        else if (r.exchange == "bybit") bybit_count++;
        else if (r.exchange == "okx") okx_count++;
        
        if (r.funding_time_ms < min_time) min_time = r.funding_time_ms;
        if (r.funding_time_ms > max_time) max_time = r.funding_time_ms;
    }
    
    // Convert timestamps to dates
    auto to_date = [](int64_t ms) -> std::string {
        time_t t = ms / 1000;
        struct tm* tm_info = localtime(&t);
        char buf[32];
        strftime(buf, 32, "%Y-%m-%d", tm_info);
        return buf;
    };
    
    int days = (max_time - min_time) / (24 * 3600 * 1000);
    
    std::cout << "║ Date Range:      " << std::setw(12) << to_date(min_time) << " to " 
              << std::setw(12) << to_date(max_time) << " (" << days << " days)      ║\n";
    std::cout << "║ Binance Records: " << std::setw(10) << binance_count << std::string(48, ' ') << "║\n";
    std::cout << "║ Bybit Records:   " << std::setw(10) << bybit_count << std::string(48, ' ') << "║\n";
    std::cout << "║ OKX Records:     " << std::setw(10) << okx_count << std::string(48, ' ') << "║\n";
    std::cout << "║ Total Records:   " << std::setw(10) << rates.size() << std::string(48, ' ') << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    
    // Show sample rates
    std::cout << "\n[SAMPLE] Last 10 funding rates:\n";
    std::cout << "  " << std::left << std::setw(10) << "Exchange" 
              << std::setw(12) << "Date" 
              << std::right << std::setw(12) << "Rate (%)" << "\n";
    std::cout << "  " << std::string(34, '-') << "\n";
    
    int shown = 0;
    for (auto it = rates.rbegin(); it != rates.rend() && shown < 10; ++it, ++shown) {
        std::cout << "  " << std::left << std::setw(10) << it->exchange
                  << std::setw(12) << to_date(it->funding_time_ms)
                  << std::right << std::setw(12) << std::fixed << std::setprecision(4) 
                  << (it->funding_rate * 100) << "\n";
    }
    
    // Run backtest
    std::cout << "\n[RUNNING] Backtest with min spread = " << min_diff_bps << " bps...\n";
    
    auto result = HistoricalFundingBacktester::backtest(rates, symbol, position_size, min_diff_bps);
    result.print();
    
    // Test different thresholds
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    SENSITIVITY ANALYSIS (REAL DATA)                         ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ " << std::left << std::setw(12) << "Min Diff" 
              << std::setw(10) << "Trades"
              << std::setw(15) << "Net P&L"
              << std::setw(12) << "Win Rate"
              << std::setw(12) << "Sharpe" 
              << "      ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    
    for (double thresh : {1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0}) {
        auto r = HistoricalFundingBacktester::backtest(rates, symbol, position_size, thresh);
        std::cout << "║ " << std::setw(10) << std::fixed << std::setprecision(1) << thresh << " bps"
                  << std::setw(10) << r.num_trades
                  << " $" << std::setw(12) << std::setprecision(2) << r.net_pnl
                  << std::setw(10) << std::setprecision(1) << (r.win_rate * 100) << "%"
                  << std::setw(12) << std::setprecision(3) << r.daily_sharpe
                  << "      ║\n";
    }
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    
    // Multi-symbol analysis
    std::cout << "\n[INFO] Testing multiple symbols...\n\n";
    
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT"};
    
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    MULTI-SYMBOL ANALYSIS (REAL DATA)                        ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ " << std::left << std::setw(12) << "Symbol" 
              << std::setw(10) << "Records"
              << std::setw(10) << "Trades"
              << std::setw(15) << "Net P&L"
              << std::setw(12) << "Win Rate"
              << "     ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    
    for (const auto& sym : symbols) {
        // Try to load from cache first
        std::string sym_cache = "data/funding_history_" + sym + ".csv";
        std::vector<HistoricalFundingRate> sym_rates;
        
        std::ifstream test(sym_cache);
        if (test.good()) {
            sym_rates = HistoricalFundingFetcher::load_from_csv(sym_cache);
        } else {
            std::cout << "║ [FETCHING " << sym << "...]" << std::string(55, ' ') << "║\n";
            sym_rates = HistoricalFundingFetcher::fetch_all_history(sym);
            if (!sym_rates.empty()) {
                HistoricalFundingFetcher::save_to_csv(sym_rates, sym_cache);
            }
        }
        
        if (!sym_rates.empty()) {
            auto r = HistoricalFundingBacktester::backtest(sym_rates, sym, position_size, min_diff_bps);
            std::cout << "║ " << std::left << std::setw(12) << sym
                      << std::setw(10) << sym_rates.size()
                      << std::setw(10) << r.num_trades
                      << " $" << std::setw(12) << std::fixed << std::setprecision(2) << r.net_pnl
                      << std::setw(10) << std::setprecision(1) << (r.win_rate * 100) << "%"
                      << "     ║\n";
        }
    }
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    
    std::cout << "\n[DONE] Backtest complete. All results based on REAL historical data.\n";
    std::cout << "[NOTE] Metrics are NOT annualized (daily Sharpe, daily returns).\n";
    
    return 0;
}
