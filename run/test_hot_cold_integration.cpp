// run/test_hot_cold_integration.cpp
// Integration Test: Hot Path + Cold Path without full exchange connectors
// Uses mock data generator instead of real WebSocket connections

#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <memory>
#include <random>
#include <chrono>

#include "../pipeline/spsc_queue.hpp"
#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalized_data.hpp"
#include "hot_path_processor_simple.hpp"
#include "cold_path_aggregator_simple.hpp"

std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\n[TEST] Caught signal " << sig << ", shutting down...\n";
    g_running.store(false);
}

// Mock data generator
class MockMarketDataGenerator {
public:
    MockMarketDataGenerator(
        const std::string& exchange,
        const std::string& product,
        std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue,
        std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> spsc_queue
    )
        : exchange_(exchange),
          product_(product),
          mpmc_queue_(mpmc_queue),
          spsc_queue_(spsc_queue),
          running_(false),
          rng_(std::random_device{}()),
          price_dist_(50000.0, 60000.0),
          size_dist_(0.1, 10.0)
    {}
    
    void start() {
        running_ = true;
        thread_ = std::thread(&MockMarketDataGenerator::generate_loop, this);
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    
private:
    void generate_loop() {
        uint64_t sequence = 0;
        
        while (running_) {
            pipeline::NormalizedQuote quote;
            quote.exchange = exchange_;
            quote.product_id = product_;
            quote.base = "BTC";
            quote.quote = "USD";
            quote.sequence = sequence++;
            quote.local_timestamp = std::chrono::system_clock::now();
            quote.exchange_timestamp = quote.local_timestamp;
            
            // Generate random market data
            double mid_price = price_dist_(rng_);
            double spread = 1.0;
            
            quote.best_bid = mid_price - spread / 2.0;
            quote.best_ask = mid_price + spread / 2.0;
            quote.bid_size = size_dist_(rng_);
            quote.ask_size = size_dist_(rng_);
            
            // Add some depth
            for (int i = 0; i < 5; i++) {
                quote.bids.push_back({quote.best_bid - i * 0.5, size_dist_(rng_)});
                quote.asks.push_back({quote.best_ask + i * 0.5, size_dist_(rng_)});
            }
            
            // Push to both queues
            mpmc_queue_->try_enqueue(quote);
            spsc_queue_->try_push(quote);
            
            // Generate ~100 quotes per second
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::string exchange_;
    std::string product_;
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue_;
    std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> spsc_queue_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> price_dist_;
    std::uniform_real_distribution<double> size_dist_;
};

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::cout << "================================================================\n";
    std::cout << "  HOT PATH + COLD PATH INTEGRATION TEST\n";
    std::cout << "  Mock Market Data → Hot Path + Cold Path Processing\n";
    std::cout << "================================================================\n\n";
    
    // Create queues
    auto mpmc_queue = std::make_shared<pipeline::MPMCQueue<pipeline::NormalizedQuote>>(1048576);
    auto coinbase_spsc = std::make_shared<pipeline::SPSCQueue<pipeline::NormalizedQuote>>(524288);
    
    std::cout << "[INIT] ✓ Queues created\n\n";
    
    // Start hot path processor
    std::cout << "[HOT PATH] Starting processor...\n";
    hft::HotPathProcessorSimple hot_path("coinbase", coinbase_spsc, -1);
    hot_path.start();
    std::cout << "[HOT PATH] ✓ Processor started\n\n";
    
    // Start cold path aggregator
    std::cout << "[COLD PATH] Starting aggregator...\n";
    hft::ColdPathAggregatorSimple cold_path(mpmc_queue, "test_integration.db", -1);
    cold_path.start();
    std::cout << "[COLD PATH] ✓ Aggregator started\n\n";
    
    // Start mock data generator
    std::cout << "[MOCK] Starting market data generator...\n";
    MockMarketDataGenerator generator("coinbase", "BTC-USD", mpmc_queue, coinbase_spsc);
    generator.start();
    std::cout << "[MOCK] ✓ Generator started (100 quotes/sec)\n\n";
    
    std::cout << "================================================================\n";
    std::cout << "  TEST RUNNING - Press Ctrl+C to stop\n";
    std::cout << "================================================================\n\n";
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Monitor loop
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        std::cout << "\n[STATS] Uptime: " << uptime << "s\n";
        std::cout << "[HOT PATH] Messages: " << hot_path.get_messages_processed()
                  << ", Signals: " << hot_path.get_signals_generated() << "\n";
        std::cout << "[COLD PATH] Arb opportunities: " << cold_path.get_arb_opportunities()
                  << ", Archived: " << cold_path.get_quotes_archived() << "\n";
    }
    
    // Shutdown
    std::cout << "\n[SHUTDOWN] Stopping components...\n";
    generator.stop();
    hot_path.stop();
    cold_path.stop();
    std::cout << "[SHUTDOWN] ✓ Complete\n";
    
    return 0;
}
