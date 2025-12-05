// run/cold_path_aggregator_simple.hpp
// Simplified Cold Path Aggregator for Integration Testing
// No complex arbitrage - just archives to SQLite

#pragma once

#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalized_data.hpp"
#include "../core/cpu_affinity.hpp"
#include "../storage/sqlite/market_data_store.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <iostream>

namespace hft {

class ColdPathAggregatorSimple {
public:
    ColdPathAggregatorSimple(
        std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue,
        const std::string& db_path,
        int archive_cpu_core = -1
    )
        : mpmc_queue_(mpmc_queue),
          db_path_(db_path),
          archive_cpu_core_(archive_cpu_core),
          running_(false),
          quotes_archived_(0)
    {
        std::cout << "[ColdPath] Initialized (simple mode)\n";
    }
    
    ~ColdPathAggregatorSimple() {
        stop();
    }
    
    void start() {
        if (running_.load()) {
            std::cerr << "[ColdPath] Already running\n";
            return;
        }
        
        running_.store(true);
        archive_thread_ = std::thread(&ColdPathAggregatorSimple::archive_loop, this);
        
        if (archive_cpu_core_ >= 0) {
#ifdef __linux__
            core::set_thread_affinity(archive_thread_, archive_cpu_core_);
            std::cout << "[ColdPath] Archive thread pinned to core " << archive_cpu_core_ << "\n";
#endif
        }
        
        std::cout << "[ColdPath] Started\n";
    }
    
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        
        if (archive_thread_.joinable()) {
            archive_thread_.join();
        }
        
        std::cout << "[ColdPath] Stopped. Archived: " << quotes_archived_.load() << " quotes\n";
    }
    
    uint64_t get_arb_opportunities() const {
        return 0; // No arb in simple mode
    }
    
    uint64_t get_quotes_archived() const {
        return quotes_archived_.load();
    }

private:
    void archive_loop() {
        // Initialize database
        storage::MarketDataStore db(db_path_);
        
        pipeline::NormalizedQuote quote;
        std::vector<pipeline::NormalizedQuote> batch;
        batch.reserve(100);
        
        while (running_.load()) {
            if (mpmc_queue_->try_dequeue(quote)) {
                batch.push_back(quote);
                
                // Batch write every 100 quotes
                if (batch.size() >= 100) {
                    for (const auto& q : batch) {
                        db.insert_quote(q);
                    }
                    quotes_archived_ += batch.size();
                    batch.clear();
                    
                    if (quotes_archived_.load() % 1000 == 0) {
                        std::cout << "[ColdPath] Archived " << quotes_archived_.load() 
                                  << " quotes\n";
                    }
                }
            } else {
                // Flush remaining batch
                if (!batch.empty()) {
                    for (const auto& q : batch) {
                        db.insert_quote(q);
                    }
                    quotes_archived_ += batch.size();
                    batch.clear();
                }
                std::this_thread::yield();
            }
        }
        
        // Final flush
        if (!batch.empty()) {
            for (const auto& q : batch) {
                db.insert_quote(q);
            }
            quotes_archived_ += batch.size();
        }
    }
    
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue_;
    std::string db_path_;
    int archive_cpu_core_;
    
    std::atomic<bool> running_;
    std::thread archive_thread_;
    std::atomic<uint64_t> quotes_archived_;
};

} // namespace hft
