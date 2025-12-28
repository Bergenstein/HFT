// run/hot_path_processor.hpp
// Per-Exchange Hot Path Processor
// Single exchange → SPSC queue → Strategy execution → ZeroMQ publish

#pragma once

#include "../pipeline/spsc_queue.hpp"
#include "../pipeline/normalizer.hpp"
#include "../core/cpu_affinity.hpp"
#include "../core/order_book.hpp"
// Strategy includes moved to sabi-cppstrategies
// #include "../strats/imbalance_taker.hpp"
// #include "../strats/strategy_ofi.hpp"
// #include "../strats/microprice_strategy.hpp"
#include "../zmq/market_data_server.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <chrono>
#include <iostream>

namespace hft {

/**
 * HotPathProcessor: Per-Exchange Real-Time Strategy Execution
 * 
 * ARCHITECTURE:
 * - One instance per exchange (Coinbase, Binance, Kraken)
 * - Dedicated thread for strategy execution
 * - CPU pinned for minimum latency
 * - SPSC queue (single producer = exchange thread)
 * 
 * DATA FLOW:
 * Exchange WebSocket Thread
 *   ↓ [NormalizedQuote]
 * SPSC Queue (Lock-Free)
 *   ↓
 * Strategy Processor Thread <-- YOU ARE HERE
 *   ├→ Reconstruct OrderBook
 *   ├→ Run Strategies (OFI, Imbalance, Microprice)
 *   └→ Publish Signals to ZeroMQ
 * 
 * WHY SEPARATE THREAD?
 * - Decouple strategy logic from network I/O
 * - Enable CPU pinning for predictable latency
 * - Isolate crashes (strategy crash doesn't kill WebSocket)
 * 
 * LATENCY TARGET: <30μs from queue read to signal publish
 */
class HotPathProcessor {
public:
    /**
     * Constructor
     * 
     * @param exchange: Exchange name ("coinbase", "binance", "kraken")
     * @param queue: SPSC queue shared with exchange WebSocket thread
     * @param zmq_endpoint: ZeroMQ publisher endpoint (e.g., "tcp://*:5555")
     * @param cpu_core: CPU core to pin thread to (-1 = no pinning)
     */
    HotPathProcessor(
        const std::string& exchange,
        std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> queue,
        const std::string& zmq_endpoint,
        int cpu_core = -1
    )
        : exchange_(exchange),
          queue_(queue),
          zmq_endpoint_(zmq_endpoint),
          cpu_core_(cpu_core),
          running_(false),
          messages_processed_(0),
          signals_generated_(0)
    {
        // Initialize strategies (Note: Using base Strategy class for now)
        // TODO: Implement concrete strategy classes or use existing ones
        // strategies_.push_back(std::make_unique<ImbalanceTaker>(0.6, 150));
        // strategies_.push_back(std::make_unique<StrategyOFI>(0.002, 100));
        // strategies_.push_back(std::make_unique<MicropriceStrategy>(0.0001, 50));
        
        std::cout << "[HotPath:" << exchange_ << "] Initialized with " 
                  << strategies_.size() << " strategies (currently disabled)\n";
    }
    
    ~HotPathProcessor() {
        stop();
    }
    
    /**
     * start: Launch strategy processor thread
     */
    void start() {
        if (running_.load()) {
            std::cerr << "[HotPath:" << exchange_ << "] Already running\n";
            return;
        }
        
        running_.store(true);
        processor_thread_ = std::thread(&HotPathProcessor::process_loop, this);
        
        // CPU pinning (if specified)
        if (cpu_core_ >= 0) {
#ifdef __linux__
            core::set_thread_affinity(processor_thread_, cpu_core_);
            std::cout << "[HotPath:" << exchange_ << "] Pinned to CPU core " 
                      << cpu_core_ << "\n";
#else
            std::cout << "[HotPath:" << exchange_ << "] CPU pinning not supported on this platform\n";
#endif
        }
        
        std::cout << "[HotPath:" << exchange_ << "] Started\n";
    }
    
    /**
     * stop: Gracefully shutdown processor thread
     */
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        
        if (processor_thread_.joinable()) {
            processor_thread_.join();
        }
        
        std::cout << "[HotPath:" << exchange_ << "] Stopped. "
                  << "Processed: " << messages_processed_.load()
                  << ", Signals: " << signals_generated_.load() << "\n";
    }
    
    /**
     * get_metrics: Retrieve performance metrics
     */
    struct Metrics {
        uint64_t messages_processed;
        uint64_t signals_generated;
        double messages_per_second;
        std::chrono::microseconds avg_latency;
    };
    
    Metrics get_metrics() const {
        return {
            messages_processed_.load(),
            signals_generated_.load(),
            0.0,  // TODO: Calculate from timing
            std::chrono::microseconds(0)  // TODO: Track latency
        };
    }

private:
    /**
     * process_loop: Main processing loop (runs in dedicated thread)
     * 
     * ALGORITHM:
     * 1. Try to dequeue NormalizedQuote from SPSC queue
     * 2. If available:
     *    a. Update local OrderBook reconstruction
     *    b. Run all strategies on new book state
     *    c. Publish signals to ZeroMQ (if generated)
     * 3. If queue empty, yield CPU (avoid busy-wait)
     * 4. Repeat until stopped
     */
    void process_loop() {
        // Initialize ZeroMQ publisher
        hft::MarketDataServer zmq_publisher(zmq_endpoint_);
        
        // Per-product order books
        std::map<std::string, core::OrderBook> orderbooks_;
        
        // Timing for latency measurement
        auto last_metrics_print = std::chrono::steady_clock::now();
        
        while (running_.load()) {
            pipeline::NormalizedQuote quote;
            
            // Try to dequeue (non-blocking)
            if (queue_->try_pop(quote)) {
                // Start latency timer
                auto start = std::chrono::high_resolution_clock::now();
                
                // Update or create orderbook for this product
                auto& ob = orderbooks_[quote.product_id];
                update_orderbook(ob, quote);
                
                // Run all strategies
                for (auto& strategy : strategies_) {
                    TickContext context;
                    context.timestamp = quote.local_timestamp;
                    context.mid_price = (quote.best_bid + quote.best_ask) / 2.0;
                    
                    int signal = strategy->on_tick(context, ob);
                    
                    if (signal != 0) {
                        // Publish signal to ZeroMQ
                        publish_signal(zmq_publisher, quote.product_id, 
                                       strategy->name(), signal, context.mid_price);
                        signals_generated_++;
                    }
                }
                
                messages_processed_++;
                
                // Measure latency
                auto end = std::chrono::high_resolution_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                
                // Warn if latency exceeds target
                if (latency.count() > 30) {
                    std::cerr << "[HotPath:" << exchange_ << "] HIGH LATENCY: " 
                              << latency.count() << "μs\n";
                }
                
            } else {
                // Queue empty - yield CPU to avoid busy-wait
                std::this_thread::yield();
            }
            
            // Print metrics every 5 seconds
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_metrics_print).count() >= 5) {
                print_metrics();
                last_metrics_print = now;
            }
        }
    }
    
    /**
     * update_orderbook: Reconstruct local OrderBook from NormalizedQuote
     * 
     * WHY RECONSTRUCT?
     * - Strategies need core::OrderBook interface
     * - NormalizedQuote is flat structure (no L2 depth methods)
     * - Lightweight: Only stores top 5 levels
     */
    void update_orderbook(core::OrderBook& ob, const pipeline::NormalizedQuote& quote) {
        // Clear existing levels
        ob.clear();
        
        // Add bid levels
        for (const auto& level : quote.bids) {
            if (level.size > 0) {
                ob.add_bid(level.price, level.size);
            }
        }
        
        // Add ask levels
        for (const auto& level : quote.asks) {
            if (level.size > 0) {
                ob.add_ask(level.price, level.size);
            }
        }
    }
    
    /**
     * publish_signal: Publish strategy signal to ZeroMQ
     * 
     * TOPIC FORMAT: "signal.<strategy>.<exchange>.<product>"
     * Example: "signal.ofi.coinbase.BTC-USD"
     */
    void publish_signal(
        hft::MarketDataServer& zmq,
        const std::string& product,
        const std::string& strategy_name,
        int signal,
        double price
    ) {
        std::string topic = "signal." + strategy_name + "." + exchange_ + "." + product;
        
        // Create Protobuf message (simplified for now - use actual proto)
        std::string payload = 
            "{\"strategy\":\"" + strategy_name + "\","
            "\"exchange\":\"" + exchange_ + "\","
            "\"product\":\"" + product + "\","
            "\"signal\":" + std::to_string(signal) + ","
            "\"price\":" + std::to_string(price) + ","
            "\"timestamp\":" + std::to_string(now_ns()) + "}";
        
        // Publish to ZeroMQ
        zmq.publish_json(topic, payload);
    }
    
    /**
     * print_metrics: Log performance statistics
     */
    void print_metrics() {
        std::cout << "[HotPath:" << exchange_ << "] "
                  << "Messages: " << messages_processed_.load()
                  << ", Signals: " << signals_generated_.load()
                  << "\n";
    }
    
    /**
     * get_messages_processed: Get total messages processed
     */
    uint64_t get_messages_processed() const {
        return messages_processed_.load();
    }
    
    /**
     * get_signals_generated: Get total signals generated
     */
    uint64_t get_signals_generated() const {
        return signals_generated_.load();
    }

private:
    std::string exchange_;
    std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> queue_;
    std::string zmq_endpoint_;
    int cpu_core_;
    
    std::atomic<bool> running_;
    std::thread processor_thread_;
    
    // Strategies
    std::vector<std::unique_ptr<Strategy>> strategies_;
    
    // Metrics
    std::atomic<uint64_t> messages_processed_;
    std::atomic<uint64_t> signals_generated_;
};

} // namespace hft
