// storage/inmem/latest_quotes.hpp - Thread-Safe In-Memory Quote Cache
#pragma once

#include "../../pipeline/normalized_data.hpp" // NormalizedQuote structure
#include <unordered_map>  // For O(1) key-value lookups
#include <shared_mutex>   // For reader-writer lock (multiple readers, single writer)
#include <string>         // For keys

namespace storage {

/**
 * LatestQuotesCache: Thread-safe in-memory cache for latest quotes
 * 
 * PURPOSE:
 * - Trading strategies need fast access to latest quotes
 * - Database queries are too slow (1-10ms)
 * - Lock-free queues don't support random access by symbol
 * - This provides O(1) lookup with thread safety
 * 
 * DESIGN PATTERN: Reader-Writer Lock
 * - Multiple threads can read simultaneously (shared_lock)
 * - Only one thread can write at a time (unique_lock)
 * - Readers don't block each other (critical for performance)
 * 
 * USE CASES:
 * - Strategy queries latest BTC quote: get("coinbase", "BTC-USD")
 * - Risk system checks all positions across exchanges
 * - Cross-exchange arbitrage compares prices
 * 
 * PERFORMANCE:
 * - Read (shared_lock): ~50-100ns (highly concurrent)
 * - Write (unique_lock): ~200-500ns (blocks readers briefly)
 * - Better than: Database (1-10ms), file I/O (10-100ms)
 * - Worse than: Lock-free structures (5-10ns), but provides random access
 */
class LatestQuotesCache {
public:
    /**
     * update: Store or update a quote in the cache
     * 
     * ALGORITHM:
     * 1. Construct unique key from exchange and product_id
     * 2. Acquire exclusive write lock (blocks all readers)
     * 3. Update the quote in hash map
     * 4. Update statistics
     * 5. Release lock (readers can proceed)
     * 
     * KEY FORMAT:
     * - "coinbase:BTC-USD"
     * - "binance:ETH-USDT"
     * - Ensures uniqueness across exchanges
     * 
     * THREAD SAFETY:
     * - unique_lock: Exclusive access, blocks all readers and writers
     * - Critical section is minimal (just map update)
     * - Lock held for ~200-500ns typically
     * 
     * @param quote: The quote to store/update
     */
    void update(const pipeline::NormalizedQuote& quote) {
        // Construct unique key: "exchange:product_id"
        std::string key = quote.exchange + ":" + quote.product_id;
        
        // Acquire exclusive write lock (blocks ALL other threads)
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        // Update the cache (overwrites previous quote for this key)
        cache_[key] = quote;
        
        // Update statistics for monitoring
        stats_[quote.exchange].total_updates++;
        stats_[quote.exchange].last_update = quote.local_timestamp;
        
        // Lock automatically released when 'lock' goes out of scope
    }
    
    /**
     * get: Retrieve the latest quote for a specific exchange and product
     * 
     * ALGORITHM:
     * 1. Construct lookup key
     * 2. Acquire shared read lock (allows concurrent readers)
     * 3. Look up key in hash map
     * 4. If found, copy quote to output parameter
     * 5. Release lock
     * 
     * THREAD SAFETY:
     * - shared_lock: Multiple readers can hold this simultaneously
     * - Blocks writers (no updates while reading)
     * - Lock held for ~50-100ns typically
     * 
     * @param exchange: Exchange name (e.g., "coinbase")
     * @param product: Product ID (e.g., "BTC-USD")
     * @param out: Output parameter where quote is written
     * @return true if quote found, false otherwise
     */
    bool get(const std::string& exchange, const std::string& product, 
             pipeline::NormalizedQuote& out) const {
        // Construct lookup key
        std::string key = exchange + ":" + product;
        
        // Acquire shared read lock (multiple readers allowed)
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        // Look up in hash map (O(1) average case)
        auto it = cache_.find(key);
        if (it == cache_.end()) return false;  // Not found
        
        // Copy quote to output parameter
        out = it->second;
        return true;  // Found
    }
    
    /**
     * get_exchange_quotes: Get all quotes for a specific exchange
     * 
     * USE CASE:
     * - Risk management: "Show me all Binance positions"
     * - Monitoring: "What's the status of all Coinbase products?"
     * - Cross-exchange analysis: Compare all BTC pairs across exchanges
     * 
     * ALGORITHM:
     * 1. Acquire shared read lock
     * 2. Iterate through all quotes in cache
     * 3. Filter by exchange name
     * 4. Return matching quotes
     * 
     * NOTE: Returns a copy (vector), not references
     * This is safe but has memory overhead for large caches
     * 
     * @param exchange: Exchange name to filter by
     * @return Vector of all quotes from that exchange
     */
    std::vector<pipeline::NormalizedQuote> get_exchange_quotes(const std::string& exchange) const {
        std::vector<pipeline::NormalizedQuote> result;
        
        // Acquire shared read lock
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        // Iterate through all cached quotes
        for (const auto& [key, quote] : cache_) {
            if (quote.exchange == exchange) {
                result.push_back(quote);  // Copy matching quotes
            }
        }
        
        return result;  // Return by value (move semantics apply)
    }
    
    /**
     * ExchangeStats: Statistics per exchange for monitoring
     * 
     * METRICS:
     * - total_updates: How many quotes received from this exchange
     * - last_update: Timestamp of most recent update
     * 
     * USE CASES:
     * - Health check: "Is Binance still sending data?"
     * - Rate monitoring: "How many updates/sec from each exchange?"
     * - Latency detection: "When was the last Coinbase update?"
     */
    struct ExchangeStats {
        uint64_t total_updates = 0;  // Total quotes received
        std::chrono::system_clock::time_point last_update;  // Last update timestamp
    };
    
    /**
     * get_stats: Get statistics for a specific exchange
     * 
     * @param exchange: Exchange name
     * @return Statistics struct (default initialized if exchange not found)
     */
    ExchangeStats get_stats(const std::string& exchange) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);  // Shared lock for reading
        auto it = stats_.find(exchange);
        return it != stats_.end() ? it->second : ExchangeStats{};
    }
    
    /**
     * size: Get total number of cached quotes
     * 
     * USE CASE: Monitoring, debugging
     * 
     * @return Number of unique (exchange, product) pairs cached
     */
    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return cache_.size();
    }

private:
    /**
     * MEMBER VARIABLES:
     * 
     * mutex_: Reader-writer lock
     * - mutable: Allows locking in const methods (get, size, etc.)
     * - shared_mutex: Optimized for many readers, few writers
     * 
     * cache_: Main storage
     * - Key: "exchange:product_id" (e.g., "coinbase:BTC-USD")
     * - Value: NormalizedQuote struct
     * - unordered_map: O(1) average lookup
     * 
     * stats_: Per-exchange statistics
     * - Key: Exchange name (e.g., "coinbase")
     * - Value: ExchangeStats struct
     * - Used for monitoring and health checks
     */
    mutable std::shared_mutex mutex_;  // Reader-writer lock
    std::unordered_map<std::string, pipeline::NormalizedQuote> cache_;  // Main quote storage
    std::unordered_map<std::string, ExchangeStats> stats_;  // Per-exchange stats
};

/**
 * DESIGN TRADE-OFFS:
 * 
 * WHY NOT LOCK-FREE?
 * - Lock-free hash maps are complex and have overhead
 * - Shared_mutex is "good enough" for this use case
 * - Read contention is low (each strategy queries different symbols)
 * 
 * WHY NOT DATABASE?
 * - Database queries take 1-10ms (too slow for HFT)
 * - This cache: 50-100ns for reads
 * - 100,000x faster
 * 
 * WHY std::unordered_map?
 * - O(1) average lookup vs O(log N) for std::map
 * - Don't need ordering (just key-value access)
 * - Better cache locality than tree structures
 * 
 * MEMORY USAGE:
 * - Each NormalizedQuote: ~500 bytes (with vectors)
 * - 1000 products: ~500 KB
 * - 10,000 products: ~5 MB
 * - Easily fits in L3 cache (8-20 MB on modern CPUs)
 */

} // namespace storage
