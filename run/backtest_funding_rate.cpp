//==============================================================================
// FUNDING RATE ARBITRAGE BACKTESTER
//==============================================================================
// Backtests the funding rate arbitrage strategy using historical data.
// Supports both real SQLite data and generated test data.
//
// Usage:
//   ./build/backtest_funding_rate [db_path]      # Use SQLite data
//   ./build/backtest_funding_rate --generate     # Use generated test data
//==============================================================================

#include <iostream>
#include <string>
#include <cstring>
#include "../bt/funding_rate_backtester.hpp"

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  <db_path>           Path to SQLite database with funding_rates table\n";
    std::cout << "  --generate [days]   Generate test data for N days (default: 30)\n";
    std::cout << "  --min-diff <bps>    Minimum funding differential to trade (default: 5)\n";
    std::cout << "  --capital <usd>     Initial capital (default: 100000)\n";
    std::cout << "  --export <path>     Export trades to CSV\n";
    std::cout << "  --help              Show this help\n";
    std::cout << "\nExample:\n";
    std::cout << "  " << prog << " --generate 60 --min-diff 3 --export trades.csv\n";
}

int main(int argc, char* argv[]) {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════════════════════╗
║                    FUNDING RATE ARBITRAGE BACKTESTER                         ║
║                                                                              ║
║  Strategy: Exploit funding rate differentials across perpetual exchanges     ║
║  Metrics: Daily (NOT annualized as per user request)                         ║
╚══════════════════════════════════════════════════════════════════════════════╝
)" << "\n";
    
    // Parse arguments
    std::string db_path;
    bool use_generated = false;
    int generate_days = 30;
    std::string export_path;
    
    bt::FundingRateBacktester::Config config;
    
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--generate") == 0) {
            use_generated = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                generate_days = std::stoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--min-diff") == 0 && i + 1 < argc) {
            config.min_funding_diff_bps = std::stod(argv[++i]);
        } else if (strcmp(argv[i], "--capital") == 0 && i + 1 < argc) {
            config.initial_capital = std::stod(argv[++i]);
        } else if (strcmp(argv[i], "--export") == 0 && i + 1 < argc) {
            export_path = argv[++i];
        } else if (argv[i][0] != '-') {
            db_path = argv[i];
        }
    }
    
    // Default to generated data if no db specified
    if (db_path.empty() && !use_generated) {
        use_generated = true;
    }
    
    // Create backtester
    bt::FundingRateBacktester backtester(config);
    
    // Load data
    if (use_generated) {
        std::cout << "[DATA] Generating " << generate_days << " days of test funding rate data...\n";
        auto data = bt::FundingRateBacktester::generate_test_data(generate_days);
        backtester.load_simulated_data(data);
        std::cout << "[DATA] Generated data for exchanges: binance, bybit, okx, dydx\n";
        std::cout << "[DATA] Symbols: BTC-USDT-PERP, ETH-USDT-PERP, SOL-USDT-PERP\n\n";
    } else {
        std::cout << "[DATA] Loading from SQLite: " << db_path << "\n";
        if (!backtester.load_from_sqlite(db_path)) {
            std::cerr << "[ERROR] Failed to load data from " << db_path << "\n";
            std::cerr << "[INFO] Try running with --generate to use simulated data\n";
            return 1;
        }
    }
    
    // Run backtest
    std::cout << "[BACKTEST] Running funding rate arbitrage backtest...\n\n";
    auto metrics = backtester.run();
    
    // Print results
    backtester.print_results();
    
    // Export if requested
    if (!export_path.empty()) {
        backtester.export_to_csv(export_path);
    }
    
    // Summary
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════════\n";
    std::cout << "SUMMARY:\n";
    std::cout << "  • Total P&L: $" << std::fixed << std::setprecision(2) << metrics.total_pnl << "\n";
    std::cout << "  • Return: " << std::setprecision(2) 
              << (metrics.total_pnl / config.initial_capital * 100) << "%\n";
    std::cout << "  • Daily Sharpe: " << std::setprecision(3) << metrics.sharpe_ratio << "\n";
    std::cout << "  • Max Drawdown: " << std::setprecision(2) << (metrics.max_drawdown * 100) << "%\n";
    std::cout << "  • Win Rate: " << std::setprecision(1) << (metrics.win_rate * 100) << "%\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════════\n";
    
    // Verdict
    if (metrics.total_pnl > 0 && metrics.sharpe_ratio > 0.5 && metrics.win_rate > 0.5) {
        std::cout << "\n✅ STRATEGY IS PROFITABLE\n";
        std::cout << "   Funding rate arbitrage generated positive returns with acceptable risk.\n";
    } else if (metrics.total_pnl > 0) {
        std::cout << "\n⚠️  STRATEGY IS MARGINALLY PROFITABLE\n";
        std::cout << "   Consider adjusting parameters (min_diff, position_size) for better risk-adjusted returns.\n";
    } else {
        std::cout << "\n❌ STRATEGY IS UNPROFITABLE\n";
        std::cout << "   Review: min_funding_diff may be too low, fees too high, or market conditions unfavorable.\n";
    }
    
    return 0;
}
