// run/hot_path_processor_simple.hpp
// Simplified Hot Path Processor for Integration Testing
// No complex strategies - just counts messages

#pragma once

#include "../pipeline/spsc_queue.hpp"
#include "../pipeline/normalized_data.hpp"
#include "../core/cpu_affinity.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <iostream>
#include <chrono>

namespace hft {

class HotPathProcessorSimple {
public:
    HotPathProcessorSimple(
        const std::string& exchange,
        std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> queue,
        int cpu_core = -1
    )
        : exchange_(exchange),
          queue_(queue),
          cpu_core_(cpu_core),
          running_(false),
          messages_processed_(0)
    {
        std::cout << "[HotPath:" << exchange_ << "] Initialized (simple mode)\n";
    }
    
    ~HotPathProcessorSimple() {
        stop();
    }
    
    void start() {
        if (running_.load()) {
            std::cerr << "[HotPath:" << exchange_ << "] Already running\n";
            return;
        }
        
        running_.store(true);
        processor_thread_ = std::thread(&HotPathProcessorSimple::process_loop, this);
        
        if (cpu_core_ >= 0) {
#ifdef __linux__
            core::set_thread_affinity(processor_thread_, cpu_core_);
            std::cout << "[HotPath:" << exchange_ << "] Pinned to CPU core " << cpu_core_ << "\n";
#endif
        }
        
        std::cout << "[HotPath:" << exchange_ << "] Started\n";
    }
    
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        
        if (processor_thread_.joinable()) {
            processor_thread_.join();
        }
        
        std::cout << "[HotPath:" << exchange_ << "] Stopped. Processed: " 
                  << messages_processed_.load() << " messages\n";
    }
    
    uint64_t get_messages_processed() const {
        return messages_processed_.load();
    }
    
    uint64_t get_signals_generated() const {
        return 0; // No strategies in simple mode
    }

private:
    void process_loop() {
        pipeline::NormalizedQuote quote;
        
        while (running_.load()) {
            if (queue_->try_pop(quote)) {
                messages_processed_++;
                
                // Simple processing: just count and verify data
                if (messages_processed_.load() % 1000 == 0) {
                    std::cout << "[HotPath:" << exchange_ << "] Processed " 
                              << messages_processed_.load() << " quotes from " 
                              << quote.product_id << "\n";
                }
            } else {
                std::this_thread::yield();
            }
        }
    }
    
    std::string exchange_;
    std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> queue_;
    int cpu_core_;
    
    std::atomic<bool> running_;
    std::thread processor_thread_;
    std::atomic<uint64_t> messages_processed_;
};

} // namespace hft
