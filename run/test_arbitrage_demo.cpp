// run/test_arbitrage_demo.cpp - Demo with guaranteed arbitrage opportunities
#include "../arb/multi_exchange_engine.hpp"
#include <iostream>

int main() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════╗
║        ARBITRAGE ENGINE DEMO - Manual Scenarios       ║
╚═══════════════════════════════════════════════════════╝
)" << "\n";

    arb::MultiExchangeEngine engine(10.0);  // 10 bps threshold
    engine.start();
    
    std::cout << "[CONFIG] Minimum profit: 10 bps\n\n";
    
    // Scenario 1: Small arbitrage (below threshold)
    std::cout << "=== SCENARIO 1: Small spread (should be filtered) ===\n";
    engine.update_quote("binance", "BTC-USDT", 42000.0, 42001.0, 1.0, 1.0);
    engine.update_quote("coinbase", "BTC-USDT", 42004.0, 42005.0, 1.0, 1.0);
    // Gross spread: ~9.5 bps, After fees (0.7%): negative
    engine.print_opportunities("BTC-USDT");
    
    // Scenario 2: Medium arbitrage (profitable)
    std::cout << "\n=== SCENARIO 2: Medium spread (PROFITABLE) ===\n";
    engine.update_quote("binance", "BTC-USDT", 42000.0, 42001.0, 1.5, 1.5);
    engine.update_quote("coinbase", "BTC-USDT", 42060.0, 42061.0, 1.0, 1.0);
    // Buy binance @42001, sell coinbase @42060
    // Gross: 140 bps, Fees: 70 bps, Net: 70 bps ✓
    engine.print_opportunities("BTC-USDT");
    
    // Scenario 3: Large arbitrage across 3 exchanges
    std::cout << "\n=== SCENARIO 3: Multi-exchange opportunities ===\n";
    engine.update_quote("binance", "ETH-USDT", 2200.0, 2201.0, 5.0, 5.0);
    engine.update_quote("kraken", "ETH-USDT", 2215.0, 2216.0, 3.0, 3.0);
    engine.update_quote("okx", "ETH-USDT", 2230.0, 2231.0, 2.0, 2.0);
    // Should find: binance->kraken, binance->okx, kraken->okx
    engine.print_opportunities("ETH-USDT");
    
    // Scenario 4: Flash crash scenario
    std::cout << "\n=== SCENARIO 4: Flash crash arbitrage ===\n";
    engine.update_quote("binance", "SOL-USDT", 100.0, 100.10, 10.0, 10.0);  // Normal
    engine.update_quote("kraken", "SOL-USDT", 95.0, 95.50, 2.0, 2.0);      // Crash
    // Buy kraken @95.50, sell binance @100.0
    // Gross: 471 bps, huge opportunity!
    engine.print_opportunities("SOL-USDT");
    
    // Scenario 5: Liquidity test
    std::cout << "\n=== SCENARIO 5: Limited liquidity ===\n";
    engine.update_quote("binance", "BTC-USDT", 42000.0, 42001.0, 0.01, 0.01);  // Tiny
    engine.update_quote("coinbase", "BTC-USDT", 42100.0, 42101.0, 100.0, 100.0);  // Large
    // Should still work but with small quantity
    engine.print_opportunities("BTC-USDT");
    
    std::cout << "\n[DEMO] Complete - All scenarios tested\n";
    engine.stop();
    return 0;
}
