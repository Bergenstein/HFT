// run/cold_path_aggregator.hpp
// Cross-Exchange Aggregator (Cold Path)
// Multiple exchanges → MPMC queue → Multi-asset strategies + Archive

#pragma once

#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalizer.hpp"
#include "../core/cpu_affinity.hpp"
#include "../arb/cross_exchange_arb.hpp"
#include "../arb/perp_spot_arb.hpp"
#include "../arb/funding_rate_arb.hpp"
#include "../arb/market_neutral_pairs.hpp"
#include "../storage/sqlite/market_data_store.hpp"
#include "../zmq/market_data_server.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <map>
#include <chrono>

namespace hft {

/**
 * ColdPathAggregator: Cross-Exchange Data Aggregation & Processing
 * 
 * PURPOSE:
 * - Aggregate data from multiple exchanges into single MPMC queue
 * - Run cross-exchange arbitrage strategies
 * - Archive data to persistent storage (SQLite)
 * - Feed exchange simulator for paper trading
 * 
 * ARCHITECTURE:
 * Multiple Exchange Threads (Producers)
 *   ├→ Coinbase
 *   ├→ Binance
 *   └→ Kraken
 *   ↓ [NormalizedQuote]
 * MPMC Queue (Multi-Producer, Multi-Consumer)
 *   ↓
 * Multiple Consumer Threads:
 *   ├→ Arbitrage Strategy Thread <-- YOU ARE HERE
 *   ├→ Archival Thread (SQLite)
 *   └→ Exchange Simulator Feed
 * 
 * WHY MPMC QUEUE?
 * - Multiple producers: Each exchange pushes independently
 * - Multiple consumers: Arb strategies + archival + simulator
 * - Lock-free: CAS-based, no mutex contention
 * 
 * LATENCY:
 * - Not latency-critical (vs hot path)
 * - Focus on correctness and data consistency
 * - Target: <100μs processing time
 */
class ColdPathAggregator {
public:
    /**
     * Constructor
     * 
     * @param mpmc_queue: Shared queue for all exchanges + consumers
     * @param zmq_arb_endpoint: ZeroMQ endpoint for arb signals
     * @param db_path: SQLite database path for archival
     * @param arb_cpu_core: CPU core for arb strategy thread
     * @param archive_cpu_core: CPU core for archive thread
     */
    ColdPathAggregator(
        std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue,
        const std::string& zmq_arb_endpoint,
        const std::string& db_path,
        int arb_cpu_core = -1,
        int archive_cpu_core = -1
    )
        : mpmc_queue_(mpmc_queue),
          zmq_arb_endpoint_(zmq_arb_endpoint),
          db_path_(db_path),
          arb_cpu_core_(arb_cpu_core),
          archive_cpu_core_(archive_cpu_core),
          running_(false),
          arb_opportunities_found_(0),
          quotes_archived_(0)
    {
        // Initialize arbitrage strategies
        cross_exchange_arb_ = std::make_unique<arb::CrossExchangeArbitrage>(15.0);
        perp_spot_arb_ = std::make_unique<arb::PerpSpotArbitrage>(25.0, 8.0, 0.001);
        funding_rate_arb_ = std::make_unique<arb::FundingRateArbitrage>(0.001, 2.5, 90);
        pairs_trading_ = std::make_unique<arb::MarketNeutralPairs>(2.0, 0.5, 3.5, 720);
        
        std::cout << "[ColdPath] Initialized with 4 arbitrage strategies\n";
    }
    
    ~ColdPathAggregator() {
        stop();
    }
    
    /**
     * start: Launch consumer threads
     */
    void start() {
        if (running_.load()) {
            std::cerr << "[ColdPath] Already running\n";
            return;
        }
        
        running_.store(true);
        
        // Thread 1: Arbitrage strategies
        arb_thread_ = std::thread(&ColdPathAggregator::arbitrage_loop, this);
        if (arb_cpu_core_ >= 0) {
#ifdef __linux__
            core::set_thread_affinity(arb_thread_, arb_cpu_core_);
            std::cout << "[ColdPath:Arb] Pinned to CPU core " << arb_cpu_core_ << "\n";
#endif
        }
        
        // Thread 2: Archival to SQLite
        archive_thread_ = std::thread(&ColdPathAggregator::archive_loop, this);
        if (archive_cpu_core_ >= 0) {
#ifdef __linux__
            core::set_thread_affinity(archive_thread_, archive_cpu_core_);
            std::cout << "[ColdPath:Archive] Pinned to CPU core " << archive_cpu_core_ << "\n";
#endif
        }
        
        std::cout << "[ColdPath] Started (2 consumer threads)\n";
    }
    
    /**
     * stop: Gracefully shutdown all threads
     */
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        
        if (arb_thread_.joinable()) arb_thread_.join();
        if (archive_thread_.joinable()) archive_thread_.join();
        
        std::cout << "[ColdPath] Stopped. "
                  << "Arb opps: " << arb_opportunities_found_.load()
                  << ", Archived: " << quotes_archived_.load() << "\n";
    }
    
    /**
     * get_metrics: Performance statistics
     */
    struct Metrics {
        uint64_t arb_opportunities;
        uint64_t quotes_archived;
        size_t queue_depth;
    };
    
    Metrics get_metrics() const {
        return {
            arb_opportunities_found_.load(),
            quotes_archived_.load(),
            0  // TODO: Add queue depth monitoring
        };
    }

private:
    /**
     * arbitrage_loop: Consumer thread for arbitrage strategies
     * 
     * ALGORITHM:
     * 1. Dequeue NormalizedQuote from MPMC
     * 2. Update per-exchange quote cache
     * 3. Check for cross-exchange arbitrage opportunities
     * 4. Check for perp-spot arbitrage (if applicable)
     * 5. Check for funding rate arbitrage
     * 6. Check for pairs trading opportunities
     * 7. Publish opportunities to ZeroMQ
     */
    void arbitrage_loop() {
        hft::MarketDataServer zmq_pub(zmq_arb_endpoint_);
        
        // Per-exchange quote cache: product → exchange → quote
        std::map<std::string, std::map<std::string, pipeline::NormalizedQuote>> quote_cache_;
        
        auto last_metrics = std::chrono::steady_clock::now();
        
        while (running_.load()) {
            pipeline::NormalizedQuote quote;
            
            if (mpmc_queue_->try_dequeue(quote)) {
                // Update cache
                quote_cache_[quote.product_id][quote.exchange] = quote;
                
                // Run cross-exchange arbitrage
                auto cross_ex_opps = find_cross_exchange_opportunities(quote_cache_[quote.product_id]);
                for (const auto& opp : cross_ex_opps) {
                    publish_arbitrage_opportunity(zmq_pub, opp);
                    arb_opportunities_found_++;
                }
                
                // TODO: Run perp-spot, funding rate, pairs trading
                // Requires additional market data (funding rates, perp prices)
                
            } else {
                std::this_thread::yield();
            }
            
            // Print metrics every 10 seconds
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_metrics).count() >= 10) {
                print_arb_metrics();
                last_metrics = now;
            }
        }
    }
    
    /**
     * archive_loop: Consumer thread for SQLite archival
     * 
     * ALGORITHM:
     * 1. Dequeue NormalizedQuote from MPMC
     * 2. Accumulate in batch (up to 100 quotes)
     * 3. Write batch to SQLite in single transaction
     * 4. Repeat
     * 
     * WHY BATCHING?
     * - SQLite is slow for single inserts
     * - Batching 100 inserts → 100x speedup
     * - Transaction overhead amortized
     */
    void archive_loop() {
        // Initialize SQLite store
        storage::MarketDataStore db(db_path_);
        
        std::vector<pipeline::NormalizedQuote> batch;
        batch.reserve(100);
        
        auto last_flush = std::chrono::steady_clock::now();
        
        while (running_.load()) {
            pipeline::NormalizedQuote quote;
            
            if (mpmc_queue_->try_dequeue(quote)) {
                batch.push_back(quote);
                
                // Flush batch if full or timeout
                auto now = std::chrono::steady_clock::now();
                bool timeout = std::chrono::duration_cast<std::chrono::seconds>(now - last_flush).count() >= 1;
                
                if (batch.size() >= 100 || timeout) {
                    // Begin transaction
                    db.begin_transaction();
                    
                    for (const auto& q : batch) {
                        db.insert_quote(q);
                        quotes_archived_++;
                    }
                    
                    db.commit_transaction();
                    batch.clear();
                    last_flush = now;
                }
                
            } else {
                // Queue empty - flush any pending batch
                if (!batch.empty()) {
                    db.begin_transaction();
                    for (const auto& q : batch) {
                        db.insert_quote(q);
                        quotes_archived_++;
                    }
                    db.commit_transaction();
                    batch.clear();
                }
                
                std::this_thread::yield();
            }
        }
        
        // Final flush on shutdown
        if (!batch.empty()) {
            db.begin_transaction();
            for (const auto& q : batch) {
                db.insert_quote(q);
            }
            db.commit_transaction();
        }
    }
    
    /**
     * find_cross_exchange_opportunities: Detect price discrepancies
     * 
     * @param quotes: Map of exchange → quote for same product
     * @return Vector of arbitrage opportunities
     */
    std::vector<arb::ArbOpportunity> find_cross_exchange_opportunities(
        const std::map<std::string, pipeline::NormalizedQuote>& quotes
    ) {
        std::vector<arb::ArbOpportunity> opportunities;
        
        // Need at least 2 exchanges to arbitrage
        if (quotes.size() < 2) return opportunities;
        
        // Convert to ExchangeQuote format
        for (auto it1 = quotes.begin(); it1 != quotes.end(); ++it1) {
            arb::ExchangeQuote eq1;
            eq1.exchange = it1->first;
            eq1.bid = it1->second.best_bid;
            eq1.ask = it1->second.best_ask;
            eq1.bid_size = it1->second.bid_size;
            eq1.ask_size = it1->second.ask_size;
            eq1.timestamp = it1->second.local_timestamp;
            
            // Update cross-exchange arb detector
            cross_exchange_arb_->update_quote(it1->second.product_id, eq1);
        }
        
        // Find opportunities for this product
        auto opps = cross_exchange_arb_->find_opportunities(quotes.begin()->second.product_id);
        
        return opps;
    }
    
    /**
     * publish_arbitrage_opportunity: Send to ZeroMQ
     */
    void publish_arbitrage_opportunity(
        hft::MarketDataServer& zmq,
        const arb::ArbOpportunity& opp
    ) {
        std::string topic = "arb.cross_exchange." + opp.product;
        
        // Create JSON payload (use actual Protobuf in production)
        std::string payload = 
            "{\"type\":\"cross_exchange\","
            "\"product\":\"" + opp.product + "\","
            "\"buy_exchange\":\"" + opp.buy_exchange + "\","
            "\"sell_exchange\":\"" + opp.sell_exchange + "\","
            "\"buy_price\":" + std::to_string(opp.buy_price) + ","
            "\"sell_price\":" + std::to_string(opp.sell_price) + ","
            "\"gross_spread_bps\":" + std::to_string(opp.gross_spread_bps) + ","
            "\"net_spread_bps\":" + std::to_string(opp.net_spread_bps) + ","
            "\"expected_profit_usd\":" + std::to_string(opp.expected_profit_usd) + ","
            "\"confidence\":" + std::to_string(opp.confidence) + "}";
        
        zmq.publish_json(topic, payload);
    }
    
    void print_arb_metrics() {
        std::cout << "[ColdPath:Arb] Opportunities found: " 
                  << arb_opportunities_found_.load() << "\n";
    }
    
    /**
     * get_arb_opportunities: Get total arbitrage opportunities found
     */
    uint64_t get_arb_opportunities() const {
        return arb_opportunities_found_.load();
    }
    
    /**
     * get_quotes_archived: Get total quotes archived to SQLite
     */
    uint64_t get_quotes_archived() const {
        return quotes_archived_.load();
    }

private:
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue_;
    std::string zmq_arb_endpoint_;
    std::string db_path_;
    int arb_cpu_core_;
    int archive_cpu_core_;
    
    std::atomic<bool> running_;
    std::thread arb_thread_;
    std::thread archive_thread_;
    
    // Arbitrage strategies
    std::unique_ptr<arb::CrossExchangeArbitrage> cross_exchange_arb_;
    std::unique_ptr<arb::PerpSpotArbitrage> perp_spot_arb_;
    std::unique_ptr<arb::FundingRateArbitrage> funding_rate_arb_;
    std::unique_ptr<arb::MarketNeutralPairs> pairs_trading_;
    
    // Metrics
    std::atomic<uint64_t> arb_opportunities_found_;
    std::atomic<uint64_t> quotes_archived_;
};

} // namespace hft
