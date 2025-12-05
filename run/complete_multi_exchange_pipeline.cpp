// run/complete_multi_exchange_pipeline.cpp
// Complete TODO Item #5: Multi-exchange data pipeline with ZeroMQ broadcasting
#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <memory>
#include "../pipeline/spsc_queue.hpp"
#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalized_data.hpp"
#include "../storage/inmem/latest_quotes.hpp"
#include "../storage/sqlite/market_data_store.hpp"
#include "../zmq/market_data_server.hpp"
#include "../exchanges/multi_exchange_connector.hpp"

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\n[MAIN] Caught signal " << sig << ", shutting down...\n";
    g_running.store(false);
}

int main() {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::cout << "========================================\n";
    std::cout << "  MULTI-EXCHANGE DATA PIPELINE (TODO #5)\n";
    std::cout << "========================================\n\n";
    
    // ===================================================================
    // STEP 1: Initialize all components
    // ===================================================================
    std::cout << "[INIT] Creating components...\n";
    
    // In-memory cache for latest quotes
    auto quote_cache = std::make_shared<storage::LatestQuotesCache>();
    
    // SQLite historical storage
    auto sqlite_store = std::make_shared<storage::MarketDataStore>("multi_exchange_data.db");
    
    // ZeroMQ publisher for broadcasting
    auto zmq_publisher = std::make_shared<hft::MarketDataServer>("tcp://*:5555");
    
    // Lock-free queues
    // CRITICAL: Using MPMCQueue for exchange_queue because we have MULTIPLE producers
    // (Coinbase thread, Binance thread, Kraken thread all pushing concurrently)
    auto exchange_queue = std::make_shared<pipeline::MPMCQueue<pipeline::NormalizedQuote>>(1048576);
    auto storage_queue = std::make_shared<pipeline::MPMCQueue<pipeline::NormalizedQuote>>(262144);
    
    std::cout << "[INIT] ✓ In-memory cache created\n";
    std::cout << "[INIT] ✓ SQLite store initialized\n";
    std::cout << "[INIT] ✓ ZeroMQ publisher ready on tcp://*:5555\n";
    std::cout << "[INIT] ✓ Lock-free queues allocated\n\n";
    
    // ===================================================================
    // STEP 2: Start Multi-Exchange Connectors
    // ===================================================================
    std::cout << "[EXCHANGE] Starting multi-exchange connectors...\n";
    
    auto multi_exchange = std::make_shared<exchanges::MultiExchangeConnector>(exchange_queue);
    
    // Coinbase products
    std::vector<std::string> coinbase_products = {
        "BTC-USD", "ETH-USD", "LTC-USD", "SOL-USD"
    };
    
    // Binance symbols (converted internally)
    std::vector<std::string> binance_symbols = {
        "BTC-USDT", "ETH-USDT", "LTC-USDT", "SOL-USDT"
    };
    
    // Kraken symbols  
    std::vector<std::string> kraken_symbols = {
        "XBT/USD", "ETH/USD", "LTC/USD", "SOL/USD"
    };
    
    // Start all exchanges in parallel
    multi_exchange->add_coinbase(coinbase_products);
    multi_exchange->add_binance(binance_symbols);
    multi_exchange->add_kraken(kraken_symbols);
    
    // Give WebSockets time to connect
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    std::cout << "[EXCHANGE] ✓ Coinbase connected (" << coinbase_products.size() << " products)\n";
    std::cout << "[EXCHANGE] ✓ Binance connected (" << binance_symbols.size() << " symbols)\n";
    std::cout << "[EXCHANGE] ✓ Kraken connected (" << kraken_symbols.size() << " symbols)\n\n";
    
    // ===================================================================
    // STEP 3: Processing Thread (HOT PATH)
    // ===================================================================
    std::cout << "[PIPELINE] Starting processing thread (hot path)...\n";
    
    std::atomic<uint64_t> quotes_processed{0};
    std::atomic<uint64_t> quotes_cached{0};
    std::atomic<uint64_t> quotes_published{0};
    std::atomic<uint64_t> quotes_queued{0};
    
    std::thread processor_thread([&]() {
        pipeline::NormalizedQuote quote;
        
        while (g_running.load()) {
            if (exchange_queue->try_dequeue(quote)) {
                quotes_processed++;
                
                // 1. Update in-memory cache (fastest - ~100ns)
                quote_cache->update(quote);
                quotes_cached++;
                
                // 2. Publish to ZeroMQ (fast - ~1μs)
                std::map<double, double> bids, asks;
                for (const auto& [p, s] : quote.bids) bids[p] = s;
                for (const auto& [p, s] : quote.asks) asks[p] = s;
                
                zmq_publisher->publish_snapshot(
                    quote.product_id, bids, asks, quote.sequence
                );
                quotes_published++;
                
                // 3. Queue for SQLite (slow path - separate thread)
                if (storage_queue->try_enqueue(quote)) {
                    quotes_queued++;
                }
            } else {
                // Queue empty - yield CPU
                std::this_thread::yield();
            }
        }
        
        std::cout << "[PROCESSOR] Stopped\n";
    });
    
    std::cout << "[PIPELINE] ✓ Processing thread started\n\n";
    
    // ===================================================================
    // STEP 4: Storage Thread (COLD PATH)
    // ===================================================================
    std::cout << "[STORAGE] Starting SQLite storage thread (cold path)...\n";
    
    std::atomic<uint64_t> quotes_stored{0};
    
    std::thread storage_thread([&]() {
        pipeline::NormalizedQuote quote;
        
        // Use batch transactions for performance
        int batch_size = 0;
        const int MAX_BATCH = 100;
        
        sqlite_store->begin_transaction();
        
        while (g_running.load() || storage_queue->size() > 0) {
            if (storage_queue->try_dequeue(quote)) {
                sqlite_store->insert_quote(quote);
                quotes_stored++;
                batch_size++;
                
                // Commit batch periodically
                if (batch_size >= MAX_BATCH) {
                    sqlite_store->commit_transaction();
                    sqlite_store->begin_transaction();
                    batch_size = 0;
                }
            } else {
                // Commit any pending
                if (batch_size > 0) {
                    sqlite_store->commit_transaction();
                    sqlite_store->begin_transaction();
                    batch_size = 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        
        // Final commit
        if (batch_size > 0) {
            sqlite_store->commit_transaction();
        }
        
        std::cout << "[STORAGE] Stopped\n";
    });
    
    std::cout << "[STORAGE] ✓ Storage thread started\n\n";
    
    // ===================================================================
    // STEP 5: Monitoring Thread
    // ===================================================================
    std::cout << "[MONITOR] Starting statistics monitor...\n\n";
    
    std::thread monitor_thread([&]() {
        auto last_time = std::chrono::steady_clock::now();
        uint64_t last_processed = 0;
        
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count();
            
            uint64_t current = quotes_processed.load();
            uint64_t delta = current - last_processed;
            double rate = elapsed > 0 ? delta / double(elapsed) : 0;
            
            std::cout << "\n========================================\n";
            std::cout << "  PIPELINE STATISTICS\n";
            std::cout << "========================================\n";
            std::cout << "Quotes processed:  " << current << " (" << rate << "/sec)\n";
            std::cout << "  - Cached:        " << quotes_cached.load() << "\n";
            std::cout << "  - Published:     " << quotes_published.load() << "\n";
            std::cout << "  - Queued:        " << quotes_queued.load() << "\n";
            std::cout << "  - Stored:        " << quotes_stored.load() << "\n";
            std::cout << "Cache size:        " << quote_cache->size() << " products\n";
            std::cout << "SQLite rows:       " << sqlite_store->get_quote_count() << "\n";
            std::cout << "Queue depths:\n";
            std::cout << "  - Exchange:      " << exchange_queue->size() << "\n";
            std::cout << "  - Storage:       " << storage_queue->size() << "\n";
            std::cout << "========================================\n\n";
            
            last_time = now;
            last_processed = current;
        }
    });
    
    std::cout << "========================================\n";
    std::cout << "  PIPELINE RUNNING\n";
    std::cout << "========================================\n";
    std::cout << "ZeroMQ feed:       tcp://*:5555\n";
    std::cout << "SQLite database:   multi_exchange_data.db\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";
    
    // ===================================================================
    // STEP 6: Wait for shutdown signal
    // ===================================================================
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    std::cout << "\n[SHUTDOWN] Stopping all threads...\n";
    
    // Stop exchange connectors
    multi_exchange->stop();
    
    // Wait for threads to finish
    if (processor_thread.joinable()) processor_thread.join();
    if (storage_thread.joinable()) storage_thread.join();
    if (monitor_thread.joinable()) monitor_thread.join();
    
    std::cout << "\n========================================\n";
    std::cout << "  FINAL STATISTICS\n";
    std::cout << "========================================\n";
    std::cout << "Total quotes processed: " << quotes_processed.load() << "\n";
    std::cout << "Total quotes cached:    " << quotes_cached.load() << "\n";
    std::cout << "Total quotes published: " << quotes_published.load() << "\n";
    std::cout << "Total quotes stored:    " << quotes_stored.load() << "\n";
    std::cout << "SQLite total rows:      " << sqlite_store->get_quote_count() << "\n";
    std::cout << "========================================\n\n";
    
    std::cout << "[MAIN] Shutdown complete\n";
    
    return 0;
}
