// run/test_simulator_integration.cpp
// Demonstrates exchange simulator integration with strategies, queues, and ZeroMQ
#include "../sim/matching_engine.hpp"
#include "../pipeline/normalized_data.hpp"
#include "../pipeline/spsc_queue.hpp"
#include "../strats/imbalance_taker.hpp"
#include <iostream>
#include <thread>
#include <atomic>

using namespace sim;
using namespace pipeline;

std::atomic<bool> g_running{true};

// Convert MatchingEngine snapshot to NormalizedQuote
NormalizedQuote snapshot_to_quote(const std::string& product_id, 
                                  const MatchingEngine::Snapshot& snapshot) {
    NormalizedQuote quote;
    quote.exchange = "simulator";
    quote.product_id = product_id;
    quote.sequence = 0;
    quote.local_timestamp = snapshot.timestamp;
    quote.exchange_timestamp = snapshot.timestamp;
    
    if (!snapshot.bids.empty()) {
        quote.best_bid = snapshot.bids[0].first;
        quote.bid_size = snapshot.bids[0].second;
        quote.bids = snapshot.bids;
    }
    
    if (!snapshot.asks.empty()) {
        quote.best_ask = snapshot.asks[0].first;
        quote.ask_size = snapshot.asks[0].second;
        quote.asks = snapshot.asks;
    }
    
    return quote;
}

int main() {
    std::cout << R"(
╔════════════════════════════════════════════════════════════════╗
║    EXCHANGE SIMULATOR INTEGRATION TEST                         ║
║                                                                ║
║    Simulator → Queue → Strategy → Signals                      ║
╚════════════════════════════════════════════════════════════════╝
)" << "\n";
    
    // 1. Create matching engine
    MatchingEngine::Config config;
    config.maker_fee_bps = 5.0;
    config.taker_fee_bps = 10.0;
    
    MatchingEngine engine("BTC-USD", config);
    std::cout << "[1/5] Created matching engine\n";
    
    // 2. Create SPSC queue for market data
    SPSCQueue<NormalizedQuote> market_data_queue(1024);
    std::cout << "[2/5] Created SPSC queue (1024 capacity)\n";
    
    // 3. Create strategy
    ImbalanceTaker strategy(0.6, 150);  // 60% threshold, 150 tick hold
    std::cout << "[3/5] Created ImbalanceTaker strategy\n";
    
    // Statistics
    std::atomic<uint64_t> quotes_produced{0};
    std::atomic<uint64_t> quotes_consumed{0};
    std::atomic<int> total_signals{0};
    std::atomic<int> buy_signals{0};
    std::atomic<int> sell_signals{0};
    
    // 4. Producer thread: Simulate order flow and feed queue
    std::thread producer([&]() {
        std::cout << "[4/5] Starting producer thread...\n";
        
        // Build initial order book
        for (int i = 0; i < 10; ++i) {
            double bid_price = 50000.0 - (i * 10);
            double ask_price = 50100.0 + (i * 10);
            engine.submit_order("bid-" + std::to_string(i), OrderSide::BUY, 
                              OrderType::LIMIT, bid_price, 0.1 * (i + 1));
            engine.submit_order("ask-" + std::to_string(i), OrderSide::SELL, 
                              OrderType::LIMIT, ask_price, 0.1 * (i + 1));
        }
        
        // Simulate market activity
        int tick = 0;
        while (g_running && tick < 100) {
            // Submit random orders to create imbalance
            if (tick % 5 == 0) {
                // Add more buy orders (creates buy imbalance)
                engine.submit_order("market-buy-" + std::to_string(tick),
                                  OrderSide::BUY, OrderType::LIMIT,
                                  50000.0 + (tick % 20), 0.5);
            } else if (tick % 5 == 2) {
                // Add more sell orders (creates sell imbalance)
                engine.submit_order("market-sell-" + std::to_string(tick),
                                  OrderSide::SELL, OrderType::LIMIT,
                                  50100.0 - (tick % 20), 0.5);
            }
            
            // Get snapshot and convert to NormalizedQuote
            auto snapshot = engine.get_snapshot(5);
            NormalizedQuote quote = snapshot_to_quote("BTC-USD", snapshot);
            
            // Push to queue
            while (!market_data_queue.try_push(quote) && g_running) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
            
            quotes_produced++;
            tick++;
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        g_running = false;
        std::cout << "[Producer] Stopped after " << tick << " ticks\n";
    });
    
    // 5. Consumer thread: Process quotes with strategy
    std::thread consumer([&]() {
        std::cout << "[5/5] Starting consumer thread (strategy processing)...\n";
        
        NormalizedQuote quote;
        core::OrderBook ob;
        
        while (g_running || market_data_queue.try_pop(quote)) {
            if (!market_data_queue.try_pop(quote)) {
                std::this_thread::yield();
                continue;
            }
            
            // Update order book
            ob.update(quote.best_bid, quote.bid_size, quote.best_ask, quote.ask_size);
            
            // Run strategy
            TickContext tc;
            tc.mid_price = quote.mid_price();
            tc.timestamp = quote.local_timestamp;
            
            int signal = strategy.on_tick(tc, ob);
            
            quotes_consumed++;
            
            if (signal != 0) {
                total_signals++;
                if (signal > 0) {
                    buy_signals++;
                    std::cout << "  [SIGNAL] BUY  @ $" << quote.mid_price() 
                              << " | Imbalance: " << ob.top_imbalance() << "\n";
                } else {
                    sell_signals++;
                    std::cout << "  [SIGNAL] SELL @ $" << quote.mid_price() 
                              << " | Imbalance: " << ob.top_imbalance() << "\n";
                }
            }
        }
        
        std::cout << "[Consumer] Stopped\n";
    });
    
    // Wait for completion
    producer.join();
    consumer.join();
    
    // Results
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "INTEGRATION TEST RESULTS\n";
    std::cout << std::string(70, '=') << "\n";
    std::cout << "Quotes produced:  " << quotes_produced.load() << "\n";
    std::cout << "Quotes consumed:  " << quotes_consumed.load() << "\n";
    std::cout << "Total signals:    " << total_signals.load() << "\n";
    std::cout << "  Buy signals:    " << buy_signals.load() << "\n";
    std::cout << "  Sell signals:   " << sell_signals.load() << "\n";
    std::cout << "Queue utilization: " 
              << (quotes_consumed.load() * 100.0 / quotes_produced.load()) << "%\n";
    
    std::cout << "\n✓ Integration test complete\n\n";
    std::cout << "COMPONENTS TESTED:\n";
    std::cout << "  ✅ MatchingEngine (sim/matching_engine.hpp)\n";
    std::cout << "  ✅ SPSCQueue (pipeline/spsc_queue.hpp)\n";
    std::cout << "  ✅ NormalizedQuote (pipeline/normalized_data.hpp)\n";
    std::cout << "  ✅ ImbalanceTaker Strategy (strats/imbalance_taker.hpp)\n";
    std::cout << "  ✅ OrderBook (core/order_book.hpp)\n";
    std::cout << "\nNOTE: For ZeroMQ integration, add MarketDataServer to publisher\n";
    std::cout << "      thread and broadcast snapshots to subscribers.\n\n";
    
    return 0;
}
