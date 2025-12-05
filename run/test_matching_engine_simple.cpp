// Simple matching engine test without WebSocket dependencies
#include "../sim/matching_engine.hpp"
#include <iostream>
#include <iomanip>

using namespace sim;

void print_snapshot(const MatchingEngine::Snapshot& snap) {
    std::cout << "\n--- Order Book Snapshot ---\n";
    std::cout << std::setw(15) << "BIDS" << " | " << std::setw(15) << "ASKS" << "\n";
    std::cout << std::setw(10) << "Price" << " " << std::setw(10) << "Size" << " | "
              << std::setw(10) << "Price" << " " << std::setw(10) << "Size" << "\n";
    std::cout << std::string(60, '-') << "\n";
    
    size_t max_depth = std::max(snap.bids.size(), snap.asks.size());
    for (size_t i = 0; i < max_depth; ++i) {
        if (i < snap.bids.size()) {
            std::cout << std::setw(10) << std::fixed << std::setprecision(2) << snap.bids[i].first
                      << " " << std::setw(10) << std::setprecision(4) << snap.bids[i].second;
        } else {
            std::cout << std::setw(22) << " ";
        }
        
        std::cout << " | ";
        
        if (i < snap.asks.size()) {
            std::cout << std::setw(10) << std::fixed << std::setprecision(2) << snap.asks[i].first
                      << " " << std::setw(10) << std::setprecision(4) << snap.asks[i].second;
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

int main() {
    std::cout << R"(
╔════════════════════════════════════════════════════════════════╗
║          MATCHING ENGINE TEST (SIMPLE)                         ║
╚════════════════════════════════════════════════════════════════╝
)" << "\n";

    // Create matching engine
    MatchingEngine::Config config;
    config.maker_fee_bps = 5.0;  // 0.05%
    config.taker_fee_bps = 10.0; // 0.10%
    
    MatchingEngine engine("BTC-USD", config);
    
    // Track fills
    int fill_count = 0;
    engine.set_fill_callback([&](const Fill& fill) {
        fill_count++;
        std::cout << "[FILL #" << fill_count << "] "
                  << (fill.side == OrderSide::BUY ? "BUY" : "SELL")
                  << " " << fill.size << " @ $" << fill.price
                  << " (fee: $" << fill.fee << ", maker: " << (fill.is_maker ? "Y" : "N") << ")\n";
    });
    
    // Test 1: Build order book with limit orders
    std::cout << "\n=== TEST 1: Building Order Book ===\n";
    
    // Add buy orders (bids)
    engine.submit_order("client-1", OrderSide::BUY, OrderType::LIMIT, 50000.00, 0.5);
    engine.submit_order("client-2", OrderSide::BUY, OrderType::LIMIT, 49950.00, 1.0);
    engine.submit_order("client-3", OrderSide::BUY, OrderType::LIMIT, 49900.00, 2.0);
    
    // Add sell orders (asks)
    engine.submit_order("client-4", OrderSide::SELL, OrderType::LIMIT, 50100.00, 0.5);
    engine.submit_order("client-5", OrderSide::SELL, OrderType::LIMIT, 50150.00, 1.0);
    engine.submit_order("client-6", OrderSide::SELL, OrderType::LIMIT, 50200.00, 2.0);
    
    auto snap1 = engine.get_snapshot(10);
    print_snapshot(snap1);
    
    std::cout << "Best Bid: $" << engine.best_bid() << "\n";
    std::cout << "Best Ask: $" << engine.best_ask() << "\n";
    std::cout << "Mid Price: $" << engine.mid_price() << "\n";
    std::cout << "Spread: $" << (engine.best_ask() - engine.best_bid()) << "\n";
    
    // Test 2: Market order crossing the spread
    std::cout << "\n=== TEST 2: Market Buy Order ===\n";
    auto order1 = engine.submit_order("client-7", OrderSide::BUY, OrderType::MARKET, 0.0, 0.3);
    
    std::cout << "Order Status: " << order_status_to_string(order1->status) << "\n";
    std::cout << "Filled: " << order1->filled_size << " / " << order1->size << "\n";
    std::cout << "Avg Price: $" << order1->avg_fill_price << "\n";
    std::cout << "Total Fees: $" << order1->total_fees << "\n";
    
    auto snap2 = engine.get_snapshot(10);
    print_snapshot(snap2);
    
    // Test 3: Limit order that crosses (aggressive limit)
    std::cout << "\n=== TEST 3: Aggressive Limit Sell ===\n";
    auto order2 = engine.submit_order("client-8", OrderSide::SELL, OrderType::LIMIT, 49990.00, 0.4);
    
    std::cout << "Order Status: " << order_status_to_string(order2->status) << "\n";
    std::cout << "Filled: " << order2->filled_size << " / " << order2->size << "\n";
    std::cout << "Remaining: " << order2->remaining_size << "\n";
    
    auto snap3 = engine.get_snapshot(10);
    print_snapshot(snap3);
    
    // Test 4: Order cancellation
    std::cout << "\n=== TEST 4: Order Cancellation ===\n";
    auto order3 = engine.submit_order("client-9", OrderSide::BUY, OrderType::LIMIT, 49800.00, 1.5);
    std::cout << "Submitted order: " << order3->order_id << "\n";
    
    bool cancelled = engine.cancel_order(order3->order_id);
    std::cout << "Cancelled: " << (cancelled ? "YES" : "NO") << "\n";
    std::cout << "Final status: " << order_status_to_string(order3->status) << "\n";
    
    // Final statistics
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "FINAL STATISTICS\n";
    std::cout << std::string(70, '=') << "\n";
    std::cout << "Total fills: " << fill_count << "\n";
    std::cout << "Best Bid: $" << engine.best_bid() << "\n";
    std::cout << "Best Ask: $" << engine.best_ask() << "\n";
    std::cout << "Mid Price: $" << engine.mid_price() << "\n";
    std::cout << "\n✓ Matching engine test complete\n\n";
    
    return 0;
}
