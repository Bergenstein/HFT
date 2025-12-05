// run/test_arbitrage.cpp - Test arbitrage detection with simulated quotes
#include "../arb/multi_exchange_engine.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <random>

void simulate_market_data(arb::MultiExchangeEngine& engine) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> price_jitter(-0.5, 0.5);
    std::uniform_real_distribution<> size_dist(0.5, 3.0);
    
    // Base prices for BTC-USDT
    double base_price = 42000.0;
    
    struct ExchangeData {
        std::string name;
        double price_offset;  // Price tends to be higher/lower
        double spread_bps;    // Typical spread in bps
    };
    
    std::vector<ExchangeData> exchanges = {
        {"binance", -5.0, 2.0},   // Usually cheaper
        {"coinbase", 10.0, 4.0},  // Usually more expensive
        {"kraken", 2.0, 6.0},     // Medium
        {"okx", -3.0, 2.5}        // Competitive
    };
    
    std::cout << "[SIMULATOR] Starting market data simulation...\n";
    
    for (int i = 0; i < 100; ++i) {
        for (const auto& exch : exchanges) {
            double mid = base_price + exch.price_offset + price_jitter(gen);
            double spread = exch.spread_bps * mid / 10000.0;
            
            double bid = mid - spread / 2.0;
            double ask = mid + spread / 2.0;
            double bid_size = size_dist(gen);
            double ask_size = size_dist(gen);
            
            engine.update_quote(exch.name, "BTC-USDT", bid, ask, bid_size, ask_size);
        }
        
        // Check for opportunities every 10 iterations
        if (i % 10 == 9) {
            engine.print_opportunities("BTC-USDT");
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Drift the base price slightly
        base_price += price_jitter(gen) * 0.1;
    }
}

int main() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════╗
║     ARBITRAGE ENGINE TEST - Simulated Market Data     ║
╚═══════════════════════════════════════════════════════╝
)" << "\n";

    // Create engine with 15 bps minimum profit
    arb::MultiExchangeEngine engine(15.0);
    engine.start();
    
    std::cout << "[CONFIG] Minimum profit threshold: 15.0 bps\n";
    std::cout << "[CONFIG] Monitoring: BTC-USDT across 4 exchanges\n\n";
    
    // Run simulation
    simulate_market_data(engine);
    
    std::cout << "\n[TEST] Simulation complete\n";
    
    engine.stop();
    return 0;
}
