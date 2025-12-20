//==============================================================================
// TEST MULTI-EXCHANGE L2 FETCHER
//==============================================================================
// Tests each exchange individually and validates data quality

#include "arb/multi_exchange_l2_fetcher.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>

using namespace arb;

void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << title << "\n";
    std::cout << std::string(80, '=') << "\n";
}

void print_market_data(const ExchangeMarketData& market) {
    std::cout << "\n  Symbol: " << market.orderbook.symbol 
              << " (Normalized: " << market.orderbook.normalized_symbol << ")\n";
    
    std::cout << "  Timestamp: " << market.orderbook.timestamp_ms << " ms\n";
    
    // Orderbook stats
    std::cout << "  L2 Orderbook:\n";
    std::cout << "    Bids: " << market.orderbook.bids.size() << " levels\n";
    std::cout << "    Asks: " << market.orderbook.asks.size() << " levels\n";
    
    if (market.orderbook.best_bid() && market.orderbook.best_ask()) {
        auto bb = *market.orderbook.best_bid();
        auto ba = *market.orderbook.best_ask();
        
        std::cout << std::fixed << std::setprecision(4);
        std::cout << "    Best Bid: " << bb.price << " x " << bb.quantity << "\n";
        std::cout << "    Best Ask: " << ba.price << " x " << ba.quantity << "\n";
        std::cout << "    Mid Price: " << market.orderbook.mid_price() << "\n";
        std::cout << "    Spread (bps): " << market.orderbook.spread_bps() << "\n";
        std::cout << "    Liquidity Imbalance: " << market.orderbook.liquidity_imbalance() << "\n";
    }
    
    // Funding rate info
    if (market.funding) {
        std::cout << "  Funding Rate:\n";
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "    Rate (8h): " << (market.funding->funding_rate * 100) << "%\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "    APY: " << market.funding->funding_rate_annual << "%\n";
        if (market.funding->mark_price > 0) {
            std::cout << "    Mark Price: " << market.funding->mark_price << "\n";
        }
        if (market.funding->next_funding_time_ms > 0) {
            double hours = market.funding->hours_until_funding();
            std::cout << "    Next Funding: " << hours << " hours\n";
        }
    } else {
        std::cout << "  Funding Rate: Not available\n";
    }
}

void test_exchange(const std::string& name, 
                   std::vector<ExchangeMarketData> (*fetch_func)(int),
                   int display_count = 3) {
    print_separator("Testing " + name);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::cout << "Fetching data from " << name << "...\n";
    auto markets = fetch_func(20);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    if (markets.empty()) {
        std::cout << "\n❌ FAILED: No data retrieved from " << name << "\n";
        return;
    }
    
    std::cout << "\n✅ SUCCESS: Retrieved " << markets.size() << " markets in " 
              << duration.count() << " ms\n";
    
    // Statistics
    int with_funding = 0;
    int valid_orderbooks = 0;
    double avg_spread = 0.0;
    
    for (const auto& market : markets) {
        if (market.funding) with_funding++;
        if (market.is_valid()) {
            valid_orderbooks++;
            avg_spread += market.orderbook.spread_bps();
        }
    }
    
    if (valid_orderbooks > 0) {
        avg_spread /= valid_orderbooks;
    }
    
    std::cout << "\nStatistics:\n";
    std::cout << "  Valid Orderbooks: " << valid_orderbooks << "/" << markets.size() << "\n";
    std::cout << "  With Funding Rate: " << with_funding << "/" << markets.size() << "\n";
    std::cout << "  Avg Spread: " << std::fixed << std::setprecision(2) 
              << avg_spread << " bps\n";
    
    // Display sample markets
    std::cout << "\nSample Markets (showing first " << display_count << "):\n";
    for (int i = 0; i < std::min(display_count, (int)markets.size()); i++) {
        print_market_data(markets[i]);
    }
}

void test_all_exchanges_parallel() {
    print_separator("Testing All Exchanges in Parallel");
    
    MultiExchangeL2Fetcher fetcher;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    auto results = fetcher.fetch_all_parallel();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "\n✅ Parallel fetch completed in " << duration.count() << " ms\n";
    
    // Summary
    print_separator("Cross-Exchange Summary");
    
    std::map<std::string, int> symbol_counts;
    
    for (const auto& [exchange, markets] : results) {
        for (const auto& market : markets) {
            symbol_counts[market.orderbook.normalized_symbol]++;
        }
    }
    
    // Find symbols available on multiple exchanges
    std::vector<std::pair<std::string, int>> multi_exchange_symbols;
    for (const auto& [symbol, count] : symbol_counts) {
        if (count > 1) {
            multi_exchange_symbols.push_back({symbol, count});
        }
    }
    
    std::sort(multi_exchange_symbols.begin(), multi_exchange_symbols.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    std::cout << "\nSymbols Available on Multiple Exchanges (Top 10):\n";
    for (int i = 0; i < std::min(10, (int)multi_exchange_symbols.size()); i++) {
        std::cout << "  " << std::setw(10) << std::left << multi_exchange_symbols[i].first 
                  << " : " << multi_exchange_symbols[i].second << " exchanges\n";
    }
    
    // Find potential arbitrage opportunities
    std::cout << "\nPotential Funding Rate Arbitrage (>20% APY spread):\n";
    
    std::map<std::string, std::vector<std::pair<std::string, double>>> funding_by_symbol;
    
    for (const auto& [exchange, markets] : results) {
        for (const auto& market : markets) {
            if (market.funding) {
                funding_by_symbol[market.orderbook.normalized_symbol].push_back(
                    {exchange, market.funding->funding_rate_annual}
                );
            }
        }
    }
    
    int arb_count = 0;
    for (const auto& [symbol, funding_list] : funding_by_symbol) {
        if (funding_list.size() < 2) continue;
        
        double min_rate = 1e9;
        double max_rate = -1e9;
        std::string min_ex, max_ex;
        
        for (const auto& [ex, rate] : funding_list) {
            if (rate < min_rate) {
                min_rate = rate;
                min_ex = ex;
            }
            if (rate > max_rate) {
                max_rate = rate;
                max_ex = ex;
            }
        }
        
        double spread = max_rate - min_rate;
        if (spread > 20.0) {
            std::cout << "  " << std::setw(10) << std::left << symbol 
                      << " : " << std::fixed << std::setprecision(2) << spread << "% APY"
                      << " (Long on " << min_ex << " @ " << min_rate << "% APY, "
                      << "Short on " << max_ex << " @ " << max_rate << "% APY)\n";
            arb_count++;
        }
    }
    
    if (arb_count == 0) {
        std::cout << "  No opportunities found with >20% APY spread\n";
    }
}

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       MULTI-EXCHANGE L2 + FUNDING RATE FETCHER TEST           ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
    
    if (argc > 1) {
        std::string exchange = argv[1];
        
        if (exchange == "binance") {
            test_exchange("Binance", MultiExchangeL2Fetcher::fetch_binance);
        } else if (exchange == "bybit") {
            test_exchange("Bybit", MultiExchangeL2Fetcher::fetch_bybit);
        } else if (exchange == "okx") {
            test_exchange("OKX", MultiExchangeL2Fetcher::fetch_okx);
        } else if (exchange == "gateio") {
            test_exchange("Gate.io", MultiExchangeL2Fetcher::fetch_gateio);
        } else if (exchange == "mexc") {
            test_exchange("MEXC", MultiExchangeL2Fetcher::fetch_mexc);
        } else if (exchange == "kucoin") {
            test_exchange("KuCoin", MultiExchangeL2Fetcher::fetch_kucoin);
        } else if (exchange == "kraken") {
            test_exchange("Kraken", MultiExchangeL2Fetcher::fetch_kraken);
        } else if (exchange == "bitget") {
            test_exchange("Bitget", MultiExchangeL2Fetcher::fetch_bitget);
        } else if (exchange == "htx") {
            test_exchange("HTX", MultiExchangeL2Fetcher::fetch_htx);
        } else if (exchange == "bingx") {
            test_exchange("BingX", MultiExchangeL2Fetcher::fetch_bingx);
        } else if (exchange == "all") {
            test_all_exchanges_parallel();
        } else {
            std::cout << "\nUnknown exchange: " << exchange << "\n";
            std::cout << "Available: binance, bybit, okx, gateio, mexc, kucoin, kraken, bitget, htx, bingx, all\n";
            return 1;
        }
    } else {
        // Test all sequentially
        test_exchange("Binance", MultiExchangeL2Fetcher::fetch_binance);
        test_exchange("Bybit", MultiExchangeL2Fetcher::fetch_bybit);
        test_exchange("OKX", MultiExchangeL2Fetcher::fetch_okx);
        test_exchange("Gate.io", MultiExchangeL2Fetcher::fetch_gateio);
        test_exchange("MEXC", MultiExchangeL2Fetcher::fetch_mexc);
        test_exchange("KuCoin", MultiExchangeL2Fetcher::fetch_kucoin);
        test_exchange("Kraken", MultiExchangeL2Fetcher::fetch_kraken);
        test_exchange("Bitget", MultiExchangeL2Fetcher::fetch_bitget);
        test_exchange("HTX", MultiExchangeL2Fetcher::fetch_htx);
        test_exchange("BingX", MultiExchangeL2Fetcher::fetch_bingx);
        
        // Then test parallel fetch
        test_all_exchanges_parallel();
    }
    
    print_separator("Test Complete");
    
    return 0;
}
