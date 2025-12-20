//==============================================================================
// DEBUG: Check if funding data is flowing through the pipeline
//==============================================================================

#include "arb/multi_exchange_integration.hpp"
#include <iostream>
#include <atomic>
#include <iomanip>

using namespace arb;

int main() {
    std::cout << "\n=== DEBUGGING FUNDING DATA PIPELINE ===\n\n";
    
    // Build system with just Binance
    auto system = MultiExchangeSystemBuilder()
        .with_binance(1000)
        .build();
    
    std::atomic<int> orderbook_count{0};
    std::atomic<int> funding_count{0};
    std::atomic<int> btc_with_funding{0};
    std::atomic<int> eth_with_funding{0};
    
    // Track what data we're getting
    system->on_market_data([&](const UnifiedMarketData& data) {
        orderbook_count++;
        
        if (data.has_funding()) {
            funding_count++;
            
            // Print first 10 symbols with funding
            if (funding_count <= 10) {
                std::cout << "[" << funding_count << "] " 
                          << data.orderbook.unified_symbol.to_string() 
                          << ": " << std::fixed << std::setprecision(2)
                          << data.funding->funding_rate_annual << "% APY\n";
            }
            
            // Check specific symbols
            std::string symbol = data.orderbook.unified_symbol.to_string();
            if (symbol == "BTC/USDT") {
                btc_with_funding++;
            }
            else if (symbol == "ETH/USDT") {
                eth_with_funding++;
            }
        }
    });
    
    system->start(false);
    std::cout << "Started Binance feeder, waiting 10 seconds...\n\n";
    
    std::this_thread::sleep_for(std::chrono::seconds(10));
    
    system->stop();
    
    std::cout << "\n=== RESULTS ===\n";
    std::cout << "Orderbook updates: " << orderbook_count << "\n";
    std::cout << "Updates with funding: " << funding_count << "\n";
    std::cout << "BTC/USDT with funding: " << btc_with_funding << "\n";
    std::cout << "ETH/USDT with funding: " << eth_with_funding << "\n";
    
    if (funding_count == 0) {
        std::cout << "\n⚠️  NO FUNDING DATA - Problem in pipeline!\n";
    } else {
        std::cout << "\n✓ Funding data is flowing through pipeline\n";
    }
    
    return 0;
}
