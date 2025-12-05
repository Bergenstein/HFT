// pipeline/hft_pipeline.hpp - Production HFT Data Pipeline
#pragma once

// Include necessary headers for the HFT pipeline
#include "spsc_queue.hpp" // Lock-free Single Producer Single Consumer queue
#include "mpmc_queue.hpp" // Lock-free Multi Producer Multi Consumer queue
#include "normalized_data.hpp" // Definitions for normalized market data
#include "../core/cpu_affinity.hpp" // Utilities for CPU pinning
#include "../core/memory_pool.hpp" // Memory pool for efficient memory management
#include "../storage/market_data_store.hpp" // Persistent storage for market data
#include <thread> // For managing threads
#include <atomic> // For atomic operations
#include <vector> // For handling collections of data

namespace pipeline {

/**
 * HFT Data Pipeline Architecture:
 * 
 * Exchange WS -> Normalizer -> SPSC Queue -> Strategy Thread (pinned CPU)
 *                                  |
 *                                  +-> MPMC Queue -> Storage Thread (pinned CPU)
 *                                  |
 *                                  +-> Indicators Thread (pinned CPU)
 * 
 * Key Features:
 * - Lock-free queues (SPSC for hot path, MPMC for fan-out)
 * - CPU pinning for deterministic latency
 * - Memory pools to avoid malloc in hot path
 * - Separate threads for different concerns
 */
class HFTPipeline {
public:
    // Configuration structure for the pipeline
    struct Config {
        size_t quote_queue_size = 1048576;  // Size of the SPSC queue for quotes (1M, power of 2 for lock-free efficiency)
        size_t trade_queue_size = 524288;   // Size of the SPSC queue for trades (512K)
        size_t storage_queue_size = 262144; // Size of the MPMC queue for storage (256K)
        
        bool enable_cpu_pinning = true; // Whether to pin threads to specific CPU cores
        bool enable_realtime_priority = false;  // Whether to set threads to real-time priority (requires root privileges)
        bool enable_storage = true; // Whether to enable persistent storage
        bool enable_indicators = true; // Whether to enable indicator calculations
        
        core::CoreAssignment cores = core::CoreAssignment::get_default(); // Default CPU core assignments
    };
    
    // Constructor: Initializes the pipeline with the given configuration
    explicit HFTPipeline(const Config& config = Config())
        : config_(config),
          running_(false), // Initialize the running flag to false
          // Initialize lock-free queues with specified capacities
          quote_queue_hot_(1048576),      // SPSC queue for quotes (hot path: exchange -> strategy)
          quote_queue_storage_(262144),   // MPMC queue for quotes (fan-out: strategy -> storage)
          trade_queue_hot_(524288),       // SPSC queue for trades
          stats_{0, 0, 0, 0, 0} {         // Initialize statistics to zero
        
        // Log the initialization details
        std::cout << "[PIPELINE] Initializing HFT data pipeline\n";
        std::cout << "[PIPELINE] Quote queue: " << quote_queue_hot_.capacity() << " slots\n";
        std::cout << "[PIPELINE] Trade queue: " << trade_queue_hot_.capacity() << " slots\n";
        std::cout << "[PIPELINE] Storage queue: " << quote_queue_storage_.capacity() << " slots\n";
        
        // Initialize the storage layer if enabled
        if (config_.enable_storage) {
            storage_ = std::make_unique<storage::MarketDataStore>("hft_data.db"); // SQLite database for persistence
        }
    }
    
    // Destructor: Stops the pipeline and cleans up resources
    ~HFTPipeline() {
        stop(); // Ensure all threads are stopped before destruction
    }
    
    // Start the pipeline by launching processing threads
    void start() {
        if (running_.load()) { // Check if the pipeline is already running
            std::cerr << "[PIPELINE] Already running\n";
            return;
        }
        
        running_.store(true); // Set the running flag to true
        
        // Start the strategy processing thread (HOT PATH - highest priority)
        strategy_thread_ = std::thread([this]() {
            if (config_.enable_cpu_pinning) {
                core::pin_thread_to_core(config_.cores.strategy_core); // Pin thread to strategy core
            }
            if (config_.enable_realtime_priority) {
                core::set_realtime_priority(); // Set real-time priority if enabled
            }
            
            process_strategy_loop(); // Run the strategy processing loop
        });
        
        // Start the storage processing thread (lower priority, different core)
        if (config_.enable_storage) {
            storage_thread_ = std::thread([this]() {
                if (config_.enable_cpu_pinning) {
                    core::pin_thread_to_core(config_.cores.storage_core); // Pin thread to storage core
                }
                
                process_storage_loop(); // Run the storage processing loop
            });
        }
        
        // Start the indicators processing thread
        if (config_.enable_indicators) {
            indicators_thread_ = std::thread([this]() {
                if (config_.enable_cpu_pinning) {
                    core::pin_thread_to_core(config_.cores.monitoring_core); // Pin thread to monitoring core
                }
                
                process_indicators_loop(); // Run the indicators processing loop
            });
        }
        
        // Log that all threads have started
        std::cout << "[PIPELINE] Started all processing threads\n";
        config_.cores.print(); // Print the core assignments
    }
    
    // Stop the pipeline by terminating all threads
    void stop() {
        if (!running_.load()) return; // If already stopped, do nothing
        
        running_.store(false); // Set the running flag to false
        
        // Join all threads to ensure they have terminated
        if (strategy_thread_.joinable()) strategy_thread_.join();
        if (storage_thread_.joinable()) storage_thread_.join();
        if (indicators_thread_.joinable()) indicators_thread_.join();
        
        print_stats(); // Print the final statistics
        std::cout << "[PIPELINE] Stopped\n";
    }
    
    /**
     * HOT PATH: Push a normalized quote from the exchange (called by the network thread)
     * - Uses the lock-free SPSC queue for minimal latency
     * - Avoids heap allocation
     */
    bool push_quote(const NormalizedQuote& quote) {
        stats_.quotes_received.fetch_add(1, std::memory_order_relaxed); // Increment the received quotes counter
        
        if (!quote_queue_hot_.try_push(quote)) { // Attempt to push the quote into the SPSC queue
            stats_.quotes_dropped.fetch_add(1, std::memory_order_relaxed); // Increment the dropped quotes counter if the queue is full
            return false; // Indicate failure
        }
        
        return true; // Indicate success
    }
    
    // Push a normalized trade into the trade queue
    bool push_trade(const NormalizedTrade& trade) {
        stats_.trades_received.fetch_add(1, std::memory_order_relaxed); // Increment the received trades counter
        
        if (!trade_queue_hot_.try_push(trade)) { // Attempt to push the trade into the SPSC queue
            stats_.trades_dropped.fetch_add(1, std::memory_order_relaxed); // Increment the dropped trades counter if the queue is full
            return false; // Indicate failure
        }
        
        return true; // Indicate success
    }
    
    // Structure to hold pipeline statistics
    struct Stats {
        std::atomic<uint64_t> quotes_received; // Total quotes received
        std::atomic<uint64_t> trades_received; // Total trades received
        std::atomic<uint64_t> quotes_processed; // Total quotes processed
        std::atomic<uint64_t> quotes_dropped; // Total quotes dropped
        std::atomic<uint64_t> storage_writes; // Total storage writes
    };
    
    // Get the current statistics
    const Stats& get_stats() const { return stats_; }
    
    // Print the current statistics to the console
    void print_stats() const {
        std::cout << "\n[PIPELINE STATS]\n"
                  << "  Quotes received:  " << stats_.quotes_received.load() << "\n"
                  << "  Quotes processed: " << stats_.quotes_processed.load() << "\n"
                  << "  Quotes dropped:   " << stats_.quotes_dropped.load() << "\n"
                  << "  Trades received:  " << stats_.trades_received.load() << "\n"
                  << "  Storage writes:   " << stats_.storage_writes.load() << "\n"
                  << "  Hot queue size:   " << quote_queue_hot_.size() << "\n";
    }

private:
    /**
     * Strategy processing loop (HOT PATH - runs on a dedicated CPU core)
     */
    void process_strategy_loop() {
        std::cout << "[STRATEGY] Processing loop started\n";
        
        NormalizedQuote quote; // Temporary storage for the quote being processed
        while (running_.load(std::memory_order_relaxed)) {
            if (quote_queue_hot_.try_pop(quote)) { // Attempt to pop a quote from the SPSC queue
                stats_.quotes_processed.fetch_add(1, std::memory_order_relaxed); // Increment the processed quotes counter
                
                // TODO: Call strategy on_quote(quote)
                // This is where your trading strategies process the quote
                
                // Fan out the quote to the storage queue (MPMC for multiple consumers)
                if (config_.enable_storage) {
                    quote_queue_storage_.try_enqueue(quote);
                }
            } else {
                // If the queue is empty, yield the CPU to other threads
                std::this_thread::yield();
            }
        }
        
        std::cout << "[STRATEGY] Processing loop stopped\n";
    }
    
    /**
     * Storage processing loop (runs on a different core)
     */
    void process_storage_loop() {
        std::cout << "[STORAGE] Processing loop started\n";
        
        NormalizedQuote quote; // Temporary storage for the quote being processed
        while (running_.load(std::memory_order_relaxed)) {
            if (quote_queue_storage_.try_dequeue(quote)) { // Attempt to dequeue a quote from the MPMC queue
                if (storage_) {
                    storage_->insert_quote(quote); // Insert the quote into persistent storage
                    stats_.storage_writes.fetch_add(1, std::memory_order_relaxed); // Increment the storage writes counter
                }
            } else {
                // If the queue is empty, sleep briefly to reduce CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        
        std::cout << "[STORAGE] Processing loop stopped\n";
    }
    
    /**
     * Technical indicators processing loop
     */
    void process_indicators_loop() {
        std::cout << "[INDICATORS] Processing loop started\n";
        
        while (running_.load(std::memory_order_relaxed)) {
            // TODO: Calculate technical indicators
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Sleep to simulate periodic calculations
        }
        
        std::cout << "[INDICATORS] Processing loop stopped\n";
    }
    
    Config config_; // Configuration for the pipeline
    std::atomic<bool> running_; // Atomic flag to indicate whether the pipeline is running
    
    // Lock-free queues
    SPSCQueue<NormalizedQuote> quote_queue_hot_;       // SPSC queue for quotes (hot path: exchange -> strategy)
    MPMCQueue<NormalizedQuote> quote_queue_storage_;   // MPMC queue for quotes (fan-out: strategy -> storage)
    SPSCQueue<NormalizedTrade> trade_queue_hot_;       // SPSC queue for trades
    
    // Processing threads
    std::thread strategy_thread_; // Thread for strategy processing
    std::thread storage_thread_; // Thread for storage processing
    std::thread indicators_thread_; // Thread for indicators processing
    
    // Storage
    std::unique_ptr<storage::MarketDataStore> storage_; // Persistent storage for market data
    
    // Statistics
    Stats stats_; // Statistics for the pipeline
};

} // namespace pipeline
