// run/test_exchange_simulator.cpp - Test the exchange simulator
#include "../sim/exchange_simulator.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <signal.h>

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\n[SIGNAL] Caught signal " << signal << ", shutting down...\n";
    g_running = false;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    std::cout << R"(
╔════════════════════════════════════════════════════════════════╗
║           EXCHANGE SIMULATOR TEST (TODO #7)                    ║
║                                                                ║
║  Testing realistic exchange simulation with matching engine    ║
║  WebSocket Server: ws://localhost:9001                         ║
╚════════════════════════════════════════════════════════════════╝
)" << "\n";

    // Configure simulator
    sim::ExchangeSimulator::SimulatorConfig config;
    config.name = "SimCoinbase";
    config.ws_port = 9001;
    config.products = {"BTC-USD", "ETH-USD", "SOL-USD"};
    config.engine_config.maker_fee_bps = 5.0;   // 0.05%
    config.engine_config.taker_fee_bps = 10.0;  // 0.10%
    config.engine_config.tick_size = 0.01;
    config.engine_config.min_order_size = 0.0001;
    config.min_latency_us = 50;
    config.max_latency_us = 500;
    config.update_interval_ms = 100;
    
    // Create simulator
    sim::ExchangeSimulator exchange(config);
    
    // Start simulator
    exchange.start();
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "TESTING ORDER SUBMISSION\n";
    std::cout << std::string(70, '=') << "\n\n";
    
    // Test 1: Submit limit orders to build order book
    std::cout << "[TEST 1] Building order book with limit orders...\n";
    
    // BTC-USD: Create a realistic order book
    // Bids (buy orders)
    exchange.submit_order("BTC-USD", "client-1", sim::OrderSide::BUY, sim::OrderType::LIMIT, 43000.00, 0.5);
    exchange.submit_order("BTC-USD", "client-2", sim::OrderSide::BUY, sim::OrderType::LIMIT, 42999.00, 1.0);
    exchange.submit_order("BTC-USD", "client-3", sim::OrderSide::BUY, sim::OrderType::LIMIT, 42998.00, 2.0);
    
    // Asks (sell orders)
    exchange.submit_order("BTC-USD", "client-4", sim::OrderSide::SELL, sim::OrderType::LIMIT, 43001.00, 0.5);
    exchange.submit_order("BTC-USD", "client-5", sim::OrderSide::SELL, sim::OrderType::LIMIT, 43002.00, 1.0);
    exchange.submit_order("BTC-USD", "client-6", sim::OrderSide::SELL, sim::OrderType::LIMIT, 43003.00, 2.0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Show order book
    auto snapshot = exchange.get_snapshot("BTC-USD", 10);
    
    std::cout << "\n[BTC-USD Order Book]\n";
    std::cout << "  Asks (Sell Orders):\n";
    for (auto it = snapshot.asks.rbegin(); it != snapshot.asks.rend(); ++it) {
        std::cout << "    $" << it->first << " : " << it->second << " BTC\n";
    }
    std::cout << "  -------------------\n";
    std::cout << "  Spread: $" << (snapshot.asks[0].first - snapshot.bids[0].first) << "\n";
    std::cout << "  -------------------\n";
    std::cout << "  Bids (Buy Orders):\n";
    for (const auto& [price, size] : snapshot.bids) {
        std::cout << "    $" << price << " : " << size << " BTC\n";
    }
    
    // Test 2: Market order matching
    std::cout << "\n[TEST 2] Market order matching...\n";
    
    auto market_buy = exchange.submit_order("BTC-USD", "market-1", 
                                           sim::OrderSide::BUY, 
                                           sim::OrderType::MARKET, 
                                           0.0, 0.3);
    
    std::cout << "  Market BUY 0.3 BTC:\n";
    std::cout << "    Order ID: " << market_buy->order_id << "\n";
    std::cout << "    Status: " << static_cast<int>(market_buy->status) << "\n";
    std::cout << "    Filled: " << market_buy->filled_size << " BTC\n";
    std::cout << "    Avg Price: $" << market_buy->avg_fill_price << "\n";
    std::cout << "    Total Fees: $" << market_buy->total_fees << "\n";
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Show updated order book
    snapshot = exchange.get_snapshot("BTC-USD", 10);
    std::cout << "\n[Updated Order Book After Market Buy]\n";
    std::cout << "  Best Ask: $" << snapshot.asks[0].first << " (" << snapshot.asks[0].second << " BTC)\n";
    std::cout << "  Best Bid: $" << snapshot.bids[0].first << " (" << snapshot.bids[0].second << " BTC)\n";
    
    // Test 3: ETH-USD
    std::cout << "\n[TEST 3] Building ETH-USD order book...\n";
    
    exchange.submit_order("ETH-USD", "eth-1", sim::OrderSide::BUY, sim::OrderType::LIMIT, 2250.00, 5.0);
    exchange.submit_order("ETH-USD", "eth-2", sim::OrderSide::BUY, sim::OrderType::LIMIT, 2249.00, 10.0);
    exchange.submit_order("ETH-USD", "eth-3", sim::OrderSide::SELL, sim::OrderType::LIMIT, 2251.00, 5.0);
    exchange.submit_order("ETH-USD", "eth-4", sim::OrderSide::SELL, sim::OrderType::LIMIT, 2252.00, 10.0);
    
    auto eth_snapshot = exchange.get_snapshot("ETH-USD", 5);
    
    std::cout << "  Best Bid: $" << eth_snapshot.bids[0].first << " (" << eth_snapshot.bids[0].second << " ETH)\n";
    std::cout << "  Best Ask: $" << eth_snapshot.asks[0].first << " (" << eth_snapshot.asks[0].second << " ETH)\n";
    std::cout << "  Spread: $" << (eth_snapshot.asks[0].first - eth_snapshot.bids[0].first) << "\n";
    
    // Test 4: Order cancellation
    std::cout << "\n[TEST 4] Testing order cancellation...\n";
    
    auto limit_order = exchange.submit_order("BTC-USD", "cancel-test", 
                                            sim::OrderSide::BUY, 
                                            sim::OrderType::LIMIT, 
                                            42500.00, 1.0);
    
    std::cout << "  Submitted order: " << limit_order->order_id << "\n";
    std::cout << "  Status: " << static_cast<int>(limit_order->status) << "\n";
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    bool cancelled = exchange.cancel_order("BTC-USD", limit_order->order_id);
    std::cout << "  Cancellation " << (cancelled ? "SUCCESS" : "FAILED") << "\n";
    
    // Keep simulator running
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "Exchange simulator running...\n";
    std::cout << "WebSocket clients can connect to: ws://localhost:9001\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << std::string(70, '=') << "\n\n";
    
    // Statistics thread
    std::thread stats_thread([&]() {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            std::cout << "\n[STATS]\n";
            for (const std::string& product : {"BTC-USD", "ETH-USD", "SOL-USD"}) {
                auto snap = exchange.get_snapshot(product, 1);
                
                if (!snap.bids.empty() && !snap.asks.empty()) {
                    double mid = (snap.bids[0].first + snap.asks[0].first) / 2.0;
                    double spread_bps = ((snap.asks[0].first - snap.bids[0].first) / mid) * 10000.0;
                    
                    std::cout << "  " << product << ": $" << mid 
                              << " (spread: " << spread_bps << " bps)\n";
                }
            }
        }
    });
    
    // Wait for signal
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    stats_thread.join();
    
    // Stop simulator
    exchange.stop();
    
    std::cout << "\n✓ Exchange simulator test completed\n\n";
    
    return 0;
}
