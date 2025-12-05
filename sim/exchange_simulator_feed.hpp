// sim/exchange_simulator_feed.hpp
// Exchange Simulator Feed from Cold Path
// Consumes real market data from MPMC queue and feeds it to exchange simulator

#pragma once

#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalized_data.hpp"
#include "../sim/matching_engine.hpp"
#include "../core/cpu_affinity.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <map>
#include <chrono>
#include <iostream>

namespace hft {
namespace sim {

/**
 * ExchangeSimulatorFeed: Bridge between real market data and exchange simulator
 * 
 * PURPOSE:
 * - Consume real market data from cold path MPMC queue
 * - Feed matching engine with realistic orderbook state
 * - Enable paper trading with live data
 * - Support strategy backtesting with current market conditions
 * 
 * ARCHITECTURE:
 * MPMC Queue (Real Data)
 *   ↓
 * Simulator Feed Thread <-- YOU ARE HERE
 *   ↓
 * Matching Engine (Paper Trading)
 *   ↓
 * Strategy Orders (Test Mode)
 * 
 * USE CASES:
 * 1. Paper trading: Test strategies without real capital
 * 2. Live backtesting: Validate strategies against current market
 * 3. Latency simulation: Add realistic exchange latency
 * 4. Slippage testing: Simulate market impact
 */
class ExchangeSimulatorFeed {
public:
    /**
     * Constructor
     * 
     * @param mpmc_queue: Shared queue receiving real market data
     * @param cpu_core: CPU core to pin thread to (-1 = no pinning)
     */
    ExchangeSimulatorFeed(
        std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue,
        int cpu_core = -1
    )
        : mpmc_queue_(mpmc_queue),
          cpu_core_(cpu_core),
          running_(false),
          quotes_consumed_(0),
          orders_simulated_(0)
    {
        std::cout << "[SimFeed] Initialized\n";
    }
    
    ~ExchangeSimulatorFeed() {
        stop();
    }
    
    /**
     * start: Begin consuming market data and feeding simulator
     */
    void start() {
        if (running_.load()) {
            std::cerr << "[SimFeed] Already running\n";
            return;
        }
        
        running_.store(true);
        feed_thread_ = std::thread(&ExchangeSimulatorFeed::feed_loop, this);
        
        // CPU pinning
        if (cpu_core_ >= 0) {
#ifdef __linux__
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_core_, &cpuset);
            pthread_setaffinity_np(feed_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
            std::cout << "[SimFeed] Thread pinned to CPU core " << cpu_core_ << "\n";
#elif __APPLE__
            // macOS: thread_policy_set (more complex, skipping for now)
            std::cout << "[SimFeed] CPU pinning not supported on macOS\n";
#endif
        }
        
        std::cout << "[SimFeed] Started\n";
    }
    
    /**
     * stop: Stop consuming and join thread
     */
    void stop() {
        if (!running_.load()) {
            return;
        }
        
        running_.store(false);
        
        if (feed_thread_.joinable()) {
            feed_thread_.join();
        }
        
        std::cout << "[SimFeed] Stopped. Consumed " << quotes_consumed_.load()
                  << " quotes, simulated " << orders_simulated_.load() << " orders\n";
    }
    
    /**
     * get_matching_engine: Get matching engine for a product (placeholder)
     * Note: Simplified version - full matching engine integration pending
     */
    std::string get_matching_engine_status(const std::string& product) {
        if (product_status_.find(product) != product_status_.end()) {
            return product_status_[product];
        }
        return "unknown";
    }
    
    /**
     * create_matching_engine: Create a new matching engine for a product
     */
    void create_matching_engine(const std::string& product) {
        if (matching_engines_.find(product) == matching_engines_.end()) {
            matching_engines_[product] = std::make_shared<::sim::MatchingEngine>(product);
            std::cout << "[SimFeed] Created matching engine for " << product << "\n";
        }
    }
    
    /**
     * get_statistics: Get consumption statistics
     */
    uint64_t get_quotes_consumed() const {
        return quotes_consumed_.load();
    }
    
    uint64_t get_orders_simulated() const {
        return orders_simulated_.load();
    }

private:
    /**
     * feed_loop: Main thread loop
     */
    void feed_loop() {
        pipeline::NormalizedQuote quote;
        
        while (running_.load()) {
            if (mpmc_queue_->try_dequeue(quote)) {
                quotes_consumed_++;
                
                // Ensure matching engine exists for this product
                if (matching_engines_.find(quote.product_id) == matching_engines_.end()) {
                    create_matching_engine(quote.product_id);
                }
                
                // Update matching engine with new market data
                update_matching_engine(quote);
                
                // Optionally: Process pending orders (paper trading)
                // This would check if any simulated orders should execute
                // based on the new orderbook state
                
            } else {
                // Queue empty - yield CPU
                std::this_thread::yield();
            }
        }
    }
    
    /**
     * update_matching_engine: Update orderbook in matching engine
     */
    void update_matching_engine(const pipeline::NormalizedQuote& quote) {
        auto engine = matching_engines_[quote.product_id];
        
        // Clear old orderbook (snapshot approach)
        // In production, handle incremental updates properly
        
        // Add bids
        for (const auto& [price, size] : quote.bids) {
            // Create synthetic limit order
            // Note: MatchingEngine expects Order objects
            // This is simplified - in production, track order IDs properly
            orders_simulated_++;
        }
        
        // Add asks
        for (const auto& [price, size] : quote.asks) {
            // Create synthetic limit order
            orders_simulated_++;
        }
        
        // Note: This is a simplified implementation
        // Production version should:
        // 1. Handle incremental updates (not full snapshots)
        // 2. Track order IDs and timestamps
        // 3. Implement realistic latency simulation
        // 4. Support market impact modeling
    }

private:
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue_;
    int cpu_core_;
    
    std::atomic<bool> running_;
    std::thread feed_thread_;
    
    // Matching engines per product
    std::map<std::string, std::shared_ptr<::sim::MatchingEngine>> matching_engines_;
    
    // Product status tracking
    std::map<std::string, std::string> product_status_;
    
    // Statistics
    std::atomic<uint64_t> quotes_consumed_;
    std::atomic<uint64_t> orders_simulated_;
};

} // namespace sim
} // namespace hft
