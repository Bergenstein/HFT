//==============================================================================
// TEST MULTI-EXCHANGE FUNDING RATE FETCHER
//==============================================================================
// Tests each exchange individually to verify connectivity and data format
//==============================================================================

#include <iostream>
#include <iomanip>
#include <chrono>
#include "../arb/multi_exchange_funding_fetcher.hpp"

using namespace arb;

void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << title << "\n";
    std::cout << std::string(80, '=') << "\n";
}

void test_exchange(const std::string& name, 
                   std::function<std::vector<ExchangeFundingData>()> fetch_func) {
    print_separator("Testing " + name);
    
    auto start = std::chrono::high_resolution_clock::now();
    auto results = fetch_func();
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    if (results.empty()) {
        std::cout << "❌ FAILED - No data returned (took " << duration.count() << "ms)\n";
        return;
    }
    
    std::cout << "✅ SUCCESS - Retrieved " << results.size() << " symbols"
              << " (took " << duration.count() << "ms)\n\n";
    
    // Show top 5 by funding rate
    std::sort(results.begin(), results.end(),
        [](const ExchangeFundingData& a, const ExchangeFundingData& b) {
            return a.funding_rate_annual > b.funding_rate_annual;
        });
    
    std::cout << "Top 5 Positive Rates:\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left << std::setw(25) << "Symbol" 
              << std::right << std::setw(15) << "8hr Rate %" 
              << std::setw(15) << "Annual %" << "\n";
    std::cout << std::string(80, '-') << "\n";
    
    for (int i = 0; i < std::min(5, (int)results.size()); ++i) {
        const auto& r = results[i];
        std::cout << std::left << std::setw(25) << r.symbol
                  << std::right << std::setw(15) << std::fixed << std::setprecision(4) 
                  << (r.funding_rate * 100)
                  << std::setw(15) << std::fixed << std::setprecision(2)
                  << r.funding_rate_annual << "\n";
    }
    
    // Show top 5 negative
    std::sort(results.begin(), results.end(),
        [](const ExchangeFundingData& a, const ExchangeFundingData& b) {
            return a.funding_rate_annual < b.funding_rate_annual;
        });
    
    std::cout << "\nTop 5 Negative Rates:\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left << std::setw(25) << "Symbol" 
              << std::right << std::setw(15) << "8hr Rate %" 
              << std::setw(15) << "Annual %" << "\n";
    std::cout << std::string(80, '-') << "\n";
    
    for (int i = 0; i < std::min(5, (int)results.size()); ++i) {
        const auto& r = results[i];
        std::cout << std::left << std::setw(25) << r.symbol
                  << std::right << std::setw(15) << std::fixed << std::setprecision(4) 
                  << (r.funding_rate * 100)
                  << std::setw(15) << std::fixed << std::setprecision(2)
                  << r.funding_rate_annual << "\n";
    }
}

void test_all_parallel() {
    print_separator("Testing All Exchanges in Parallel");
    
    MultiExchangeFundingFetcher fetcher;
    
    auto start = std::chrono::high_resolution_clock::now();
    auto all_data = fetcher.fetch_all_parallel();
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    int total_symbols = 0;
    for (const auto& [ex, data] : all_data) {
        total_symbols += data.size();
    }
    
    std::cout << "\n✅ Fetched " << total_symbols << " total symbols from " 
              << all_data.size() << " exchanges in " << duration.count() << "ms\n";
    
    // Test arbitrage finder
    print_separator("Finding Cross-Exchange Arbitrage Opportunities");
    
    auto opportunities = fetcher.find_arbitrage_opportunities(20.0);
    
    if (opportunities.empty()) {
        std::cout << "ℹ️  No arbitrage opportunities found with >20% APY spread\n";
    } else {
        std::cout << "✅ Found " << opportunities.size() << " arbitrage opportunities!\n\n";
        
        std::cout << "Top 10 Opportunities:\n";
        std::cout << std::string(120, '=') << "\n";
        std::cout << std::left << std::setw(15) << "Symbol"
                  << std::setw(12) << "Long Ex"
                  << std::right << std::setw(12) << "Long APY %"
                  << "  "
                  << std::left << std::setw(12) << "Short Ex"
                  << std::right << std::setw(12) << "Short APY %"
                  << std::setw(12) << "Spread %"
                  << std::setw(10) << "# Exch" << "\n";
        std::cout << std::string(120, '=') << "\n";
        
        for (int i = 0; i < std::min(10, (int)opportunities.size()); ++i) {
            const auto& opp = opportunities[i];
            std::cout << std::left << std::setw(15) << opp.base_symbol
                      << std::setw(12) << opp.long_exchange
                      << std::right << std::setw(12) << std::fixed << std::setprecision(2)
                      << opp.long_rate_annual << "  "
                      << std::left << std::setw(12) << opp.short_exchange
                      << std::right << std::setw(12) << std::fixed << std::setprecision(2)
                      << opp.short_rate_annual
                      << std::setw(12) << std::fixed << std::setprecision(2)
                      << opp.spread_annual
                      << std::setw(10) << opp.num_exchanges << "\n";
        }
        
        // Show execution strategy for best opportunity
        if (!opportunities.empty()) {
            const auto& best = opportunities[0];
            
            print_separator("Best Opportunity Execution Plan");
            
            std::cout << "Asset: " << best.base_symbol << "\n";
            std::cout << "Expected Spread: " << std::fixed << std::setprecision(2) 
                      << best.spread_annual << "% APY\n\n";
            
            std::cout << "Position:\n";
            std::cout << "  LONG  on " << best.long_exchange << " (" << best.long_symbol << ")\n";
            std::cout << "        Funding: " << std::fixed << std::setprecision(2) 
                      << best.long_rate_annual << "% APY\n";
            std::cout << "        Fee: " << std::fixed << std::setprecision(3)
                      << (ExchangeFees::get(best.long_exchange).maker_fee * 100) << "% maker\n\n";
            
            std::cout << "  SHORT on " << best.short_exchange << " (" << best.short_symbol << ")\n";
            std::cout << "        Funding: " << std::fixed << std::setprecision(2) 
                      << best.short_rate_annual << "% APY\n";
            std::cout << "        Fee: " << std::fixed << std::setprecision(3)
                      << (ExchangeFees::get(best.short_exchange).taker_fee * 100) << "% taker\n\n";
            
            // Calculate P&L for $100k position
            double position_size = 100000.0;
            double funding_pnl_annual = best.spread_annual / 100.0 * position_size;
            double funding_pnl_daily = funding_pnl_annual / 365.0;
            
            double entry_fees = position_size * 
                (ExchangeFees::get(best.long_exchange).maker_fee + 
                 ExchangeFees::get(best.short_exchange).taker_fee);
            double exit_fees = entry_fees;
            
            std::cout << "P&L Analysis (per $100k position):\n";
            std::cout << "  Funding P&L:    $" << std::fixed << std::setprecision(2) 
                      << funding_pnl_annual << "/year ($" << funding_pnl_daily << "/day)\n";
            std::cout << "  Entry Fees:     $" << entry_fees << "\n";
            std::cout << "  Exit Fees:      $" << exit_fees << "\n";
            std::cout << "  Break-even:     " << std::fixed << std::setprecision(1)
                      << ((entry_fees + exit_fees) / funding_pnl_daily) << " days\n";
            std::cout << "  Net Annual:     $" << std::fixed << std::setprecision(2)
                      << (funding_pnl_annual - entry_fees - exit_fees) << "\n";
        }
    }
}

int main(int argc, char* argv[]) {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║         MULTI-EXCHANGE FUNDING RATE FETCHER - INTEGRATION TEST            ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝\n";
    
    if (argc > 1 && std::string(argv[1]) == "--exchange") {
        if (argc < 3) {
            std::cout << "\nUsage: " << argv[0] << " --exchange <name>\n";
            std::cout << "Available exchanges: binance, bybit, okx, gateio, mexc, kucoin, kraken\n";
            return 1;
        }
        
        std::string exchange = argv[2];
        
        if (exchange == "binance") {
            test_exchange("Binance", MultiExchangeFundingFetcher::fetch_binance);
        } else if (exchange == "bybit") {
            test_exchange("Bybit", MultiExchangeFundingFetcher::fetch_bybit);
        } else if (exchange == "okx") {
            test_exchange("OKX", MultiExchangeFundingFetcher::fetch_okx);
        } else if (exchange == "gateio") {
            test_exchange("Gate.io", MultiExchangeFundingFetcher::fetch_gateio);
        } else if (exchange == "mexc") {
            test_exchange("MEXC", MultiExchangeFundingFetcher::fetch_mexc);
        } else if (exchange == "kucoin") {
            test_exchange("KuCoin", MultiExchangeFundingFetcher::fetch_kucoin);
        } else if (exchange == "kraken") {
            test_exchange("Kraken", MultiExchangeFundingFetcher::fetch_kraken);
        } else {
            std::cout << "Unknown exchange: " << exchange << "\n";
            return 1;
        }
    } else {
        // Test all exchanges
        std::cout << "\nTesting individual exchanges...\n";
        
        test_exchange("Binance", MultiExchangeFundingFetcher::fetch_binance);
        test_exchange("Bybit", MultiExchangeFundingFetcher::fetch_bybit);
        test_exchange("OKX", MultiExchangeFundingFetcher::fetch_okx);
        test_exchange("Gate.io", MultiExchangeFundingFetcher::fetch_gateio);
        test_exchange("MEXC", MultiExchangeFundingFetcher::fetch_mexc);
        test_exchange("KuCoin", MultiExchangeFundingFetcher::fetch_kucoin);
        test_exchange("Kraken", MultiExchangeFundingFetcher::fetch_kraken);
        
        // Test parallel fetch
        test_all_parallel();
    }
    
    print_separator("Test Complete");
    
    return 0;
}
