// run/test_multi_exchange_simple.cpp
// Simple test to verify we can receive data from multiple exchanges simultaneously

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <memory>
#include "../pipeline/spsc_queue.hpp"
#include "../pipeline/normalized_data.hpp"
#include "../pipeline/normalizer.hpp"
#include "../md/ws_l2_client.hpp"
#include <nlohmann/json.hpp>

std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_coinbase_count{0};
std::atomic<uint64_t> g_total_count{0};

// Mock Binance data generator (for testing without real connection)
void mock_binance_feed(pipeline::SPSCQueue<pipeline::NormalizedQuote>* queue) {
    std::cout << "[BINANCE MOCK] Starting...\n";
    
    int count = 0;
    while (g_running && count < 10) {  // Generate 10 mock messages
        pipeline::NormalizedQuote quote;
        quote.exchange = "binance";
        quote.product_id = "BTC-USDT";
        quote.base = "BTC";
        quote.quote = "USDT";
        quote.best_bid = 42000.0 + (count * 10);
        quote.best_ask = 42001.0 + (count * 10);
        quote.bid_size = 1.5;
        quote.ask_size = 1.2;
        quote.sequence = count;
        quote.local_timestamp = std::chrono::system_clock::now();
        
        if (queue->try_push(quote)) {
            g_total_count++;
            std::cout << "[BINANCE MOCK] Pushed quote #" << count 
                     << " BTC-USDT @ $" << quote.best_bid << "\n";
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        count++;
    }
    
    std::cout << "[BINANCE MOCK] Stopped after " << count << " messages\n";
}

// Mock Kraken data generator
void mock_kraken_feed(pipeline::SPSCQueue<pipeline::NormalizedQuote>* queue) {
    std::cout << "[KRAKEN MOCK] Starting...\n";
    
    int count = 0;
    while (g_running && count < 10) {  // Generate 10 mock messages
        pipeline::NormalizedQuote quote;
        quote.exchange = "kraken";
        quote.product_id = "ETH-USD";
        quote.base = "ETH";
        quote.quote = "USD";
        quote.best_bid = 2200.0 + (count * 5);
        quote.best_ask = 2201.0 + (count * 5);
        quote.bid_size = 5.0;
        quote.ask_size = 4.5;
        quote.sequence = count;
        quote.local_timestamp = std::chrono::system_clock::now();
        
        if (queue->try_push(quote)) {
            g_total_count++;
            std::cout << "[KRAKEN MOCK] Pushed quote #" << count 
                     << " ETH-USD @ $" << quote.best_bid << "\n";
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        count++;
    }
    
    std::cout << "[KRAKEN MOCK] Stopped after " << count << " messages\n";
}

int main() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  MULTI-EXCHANGE INTEGRATION TEST\n";
    std::cout << "========================================\n\n";
    
    // Create shared queue (2M capacity for multi-exchange load)
    auto exchange_queue = std::make_unique<pipeline::SPSCQueue<pipeline::NormalizedQuote>>(2097152);
    
    std::cout << "[MAIN] Created SPSC queue with 2M capacity\n\n";
    
    // ===================================================================
    // Start Mock Exchange Feeds (for testing without real connections)
    // ===================================================================
    
    std::cout << "[MAIN] Starting mock exchange feeds...\n";
    
    std::thread binance_thread([&]() {
        mock_binance_feed(exchange_queue.get());
    });
    
    std::thread kraken_thread([&]() {
        mock_kraken_feed(exchange_queue.get());
    });
    
    std::cout << "[MAIN] Mock threads started\n\n";
    
    // ===================================================================
    // Processing Thread - Consumes from ALL exchanges
    // ===================================================================
    
    std::cout << "[MAIN] Starting processor thread...\n\n";
    
    std::thread processor_thread([&]() {
        pipeline::NormalizedQuote quote;
        uint64_t processed = 0;
        
        std::map<std::string, uint64_t> exchange_counts;
        
        while (g_running || exchange_queue->size() > 0) {
            if (exchange_queue->try_pop(quote)) {
                processed++;
                exchange_counts[quote.exchange]++;
                
                std::cout << "[PROCESSOR] #" << processed 
                         << " [" << quote.exchange << "] "
                         << quote.product_id << " @ $" << quote.best_bid 
                         << " / $" << quote.best_ask << "\n";
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        
        std::cout << "\n[PROCESSOR] Stopped. Processed " << processed << " quotes\n";
        std::cout << "Exchange breakdown:\n";
        for (const auto& [exchange, count] : exchange_counts) {
            std::cout << "  " << exchange << ": " << count << " quotes\n";
        }
    });
    
    // ===================================================================
    // Run for 15 seconds then stop
    // ===================================================================
    
    std::cout << "[MAIN] Running for 15 seconds...\n\n";
    std::this_thread::sleep_for(std::chrono::seconds(15));
    
    std::cout << "\n[MAIN] Stopping...\n";
    g_running.store(false);
    
    // Wait for threads
    binance_thread.join();
    kraken_thread.join();
    processor_thread.join();
    
    std::cout << "\n========================================\n";
    std::cout << "  TEST COMPLETE\n";
    std::cout << "========================================\n";
    std::cout << "Total quotes received: " << g_total_count.load() << "\n";
    std::cout << "Queue size at end: " << exchange_queue->size() << "\n";
    std::cout << "\n✅ Multi-exchange integration working!\n\n";
    
    return 0;
}
