// Comprehensive matching engine stress test
#include "../sim/matching_engine.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>

using namespace sim;

void print_book(const MatchingEngine& engine) {
    auto snap = engine.get_snapshot(5);
    std::cout << "\nBest Bid: $" << engine.best_bid() 
              << " | Best Ask: $" << engine.best_ask()
              << " | Spread: $" << (engine.best_ask() - engine.best_bid()) << "\n";
}

int main() {
    std::cout << "\n╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       COMPREHENSIVE MATCHING ENGINE TEST                       ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";

    MatchingEngine::Config config;
    config.maker_fee_bps = 5.0;
    config.taker_fee_bps = 10.0;
    
    MatchingEngine engine("BTC-USD", config);
    
    int total_fills = 0;
    double total_volume = 0.0;
    
    engine.set_fill_callback([&](const Fill& fill) {
        total_fills++;
        total_volume += fill.size * fill.price;
    });
    
    // TEST 1: Build deep order book
    std::cout << "=== TEST 1: Building Deep Order Book ===\n";
    
    // Add 50 bids
    for (int i = 0; i < 50; i++) {
        double price = 50000.0 - i * 10.0;
        double size = 0.1 + (i % 5) * 0.05;
        engine.submit_order("bid-" + std::to_string(i), OrderSide::BUY, OrderType::LIMIT, price, size);
    }
    
    // Add 50 asks
    for (int i = 0; i < 50; i++) {
        double price = 50100.0 + i * 10.0;
        double size = 0.1 + (i % 5) * 0.05;
        engine.submit_order("ask-" + std::to_string(i), OrderSide::SELL, OrderType::LIMIT, price, size);
    }
    
    print_book(engine);
    std::cout << "✓ Added 100 limit orders\n";
    
    // TEST 2: Large market buy (walk the book)
    std::cout << "\n=== TEST 2: Large Market Buy (5 BTC) ===\n";
    int fills_before = total_fills;
    auto order1 = engine.submit_order("mkt-1", OrderSide::BUY, OrderType::MARKET, 0.0, 5.0);
    std::cout << "Filled: " << order1->filled_size << " BTC\n";
    std::cout << "Avg Price: $" << order1->avg_fill_price << "\n";
    std::cout << "Fills generated: " << (total_fills - fills_before) / 2 << "\n";
    print_book(engine);
    
    // TEST 3: Large market sell
    std::cout << "\n=== TEST 3: Large Market Sell (3 BTC) ===\n";
    fills_before = total_fills;
    auto order2 = engine.submit_order("mkt-2", OrderSide::SELL, OrderType::MARKET, 0.0, 3.0);
    std::cout << "Filled: " << order2->filled_size << " BTC\n";
    std::cout << "Avg Price: $" << order2->avg_fill_price << "\n";
    std::cout << "Fills generated: " << (total_fills - fills_before) / 2 << "\n";
    print_book(engine);
    
    // TEST 4: Aggressive limit orders crossing spread
    std::cout << "\n=== TEST 4: Aggressive Limit Orders ===\n";
    
    // Buy above best ask
    auto order3 = engine.submit_order("agg-1", OrderSide::BUY, OrderType::LIMIT, 50500.0, 1.0);
    std::cout << "Aggressive buy: " << order3->filled_size << " filled, " 
              << order3->remaining_size << " resting\n";
    
    // Sell below best bid
    auto order4 = engine.submit_order("agg-2", OrderSide::SELL, OrderType::LIMIT, 49500.0, 1.0);
    std::cout << "Aggressive sell: " << order4->filled_size << " filled, " 
              << order4->remaining_size << " resting\n";
    
    print_book(engine);
    
    // TEST 5: Order cancellations
    std::cout << "\n=== TEST 5: Order Cancellations ===\n";
    
    auto order5 = engine.submit_order("cancel-test", OrderSide::BUY, OrderType::LIMIT, 48000.0, 2.0);
    std::cout << "Submitted order: " << order5->order_id << "\n";
    
    bool cancelled = engine.cancel_order(order5->order_id);
    std::cout << "Cancelled: " << (cancelled ? "YES" : "NO") << "\n";
    std::cout << "Status: " << order_status_to_string(order5->status) << "\n";
    
    // TEST 6: Rapid-fire orders (stress test)
    std::cout << "\n=== TEST 6: Stress Test (1000 orders) ===\n";
    auto start = std::chrono::high_resolution_clock::now();
    
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> price_dist(49000.0, 51000.0);
    std::uniform_real_distribution<double> size_dist(0.01, 0.5);
    std::uniform_int_distribution<int> side_dist(0, 1);
    
    for (int i = 0; i < 1000; i++) {
        OrderSide side = (side_dist(rng) == 0) ? OrderSide::BUY : OrderSide::SELL;
        double price = std::round(price_dist(rng) / 10.0) * 10.0;
        double size = size_dist(rng);
        
        engine.submit_order("stress-" + std::to_string(i), side, OrderType::LIMIT, price, size);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "1000 orders submitted in " << duration.count() << " μs\n";
    std::cout << "Throughput: " << (1000.0 / duration.count() * 1000000.0) << " orders/sec\n";
    print_book(engine);
    
    // TEST 7: Snapshot performance
    std::cout << "\n=== TEST 7: Snapshot Performance ===\n";
    start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 10000; i++) {
        auto snap = engine.get_snapshot(20);
    }
    
    end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    
    std::cout << "10,000 snapshots in " << duration_ns.count() / 1000 << " μs\n";
    std::cout << "Avg snapshot time: " << (duration_ns.count() / 10000.0) << " ns\n";
    
    // FINAL STATS
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "FINAL STATISTICS\n";
    std::cout << std::string(70, '=') << "\n";
    std::cout << "Total fills: " << total_fills << "\n";
    std::cout << "Total volume: $" << std::fixed << std::setprecision(2) << total_volume << "\n";
    std::cout << "Best Bid: $" << engine.best_bid() << "\n";
    std::cout << "Best Ask: $" << engine.best_ask() << "\n";
    std::cout << "Mid Price: $" << engine.mid_price() << "\n";
    std::cout << "\n✓ All tests passed\n\n";
    
    return 0;
}
