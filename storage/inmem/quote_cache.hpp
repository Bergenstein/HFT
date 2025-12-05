// storage/inmem/quote_cache.hpp - Multi-Exchange Quote Cache with Product Aggregation
#pragma once

#include "../../pipeline/normalized_data.hpp" // NormalizedQuote structure
#include <map>           // For ordered key-value storage
#include <mutex>         // For thread synchronization
#include <shared_mutex>  // For reader-writer lock
#include <optional>      // For safe optional returns

namespace storage {

/**
 * QuoteCache: Thread-safe cache for latest quotes with product aggregation
 * 
 * DIFFERENCE FROM LatestQuotesCache:
 * - This uses std::map (ordered) vs std::unordered_map (unordered)
 * - Provides product aggregation: get_all_for_product()
 * - Provides product enumeration: get_all_products()
 * - Returns std::optional instead of bool + output parameter
 * 
 * WHY std::map INSTEAD OF std::unordered_map?
 * - Ordered iteration when listing products
 * - Slightly slower lookup (O(log N) vs O(1))
 * - But N is small (typically < 100 products)
 * - Log2(100) = ~7 comparisons (still very fast)
 * 
 * USE CASES:
 * - Cross-exchange arbitrage: get_all_for_product("BTC-USD") returns all exchanges
 * - Strategy monitoring: List all available products
 * - Health checks: Verify each exchange has fresh quotes
 * 
 * THREAD SAFETY:
 * - Reader-writer lock (shared_mutex)
 * - Multiple concurrent readers allowed
 * - Single writer blocks all readers
 */
class QuoteCache {
public:
    /**
     * update: Store or update a quote in the cache
     * 
     * ALGORITHM:
     * 1. Acquire exclusive write lock
     * 2. Construct unique key from exchange and product
     * 3. Update or insert quote in map
     * 4. Increment update counter (for monitoring)
     * 5. Release lock
     * 
     * ATOMICITY:
     * - unique_lock ensures only one thread writes at a time
     * - Readers are blocked during write
     * - Write typically completes in < 500ns
     * 
     * @param quote: The quote to store/update
     */
    void update(const pipeline::NormalizedQuote& quote) {
        std::unique_lock lock(mutex_);  // Exclusive write lock
        
        // Construct key: "exchange:product_id" (e.g., "coinbase:BTC-USD")
        std::string key = make_key(quote.exchange, quote.product_id);
        
        // Insert or update the quote (map::operator[] does both)
        cache_[key] = quote;
        
        // Increment total update counter (atomic, no lock needed for read)
        update_count_++;
    }
    
    /**
     * get: Retrieve a specific quote by exchange and product
     * 
     * ALGORITHM:
     * 1. Acquire shared read lock (allows concurrent readers)
     * 2. Construct lookup key
     * 3. Search map (O(log N) for std::map)
     * 4. Return std::optional (empty if not found)
     * 5. Release lock
     * 
     * WHY std::optional?
     * - Modern C++ alternative to bool + output parameter
     * - Clearer intent: "might not exist"
     * - Usage: if (auto quote = cache.get(...)) { use *quote; }
     * 
     * @param exchange: Exchange name (e.g., "coinbase")
     * @param product: Product ID (e.g., "BTC-USD")
     * @return std::optional<Quote>: Quote if found, std::nullopt otherwise
     */
    std::optional<pipeline::NormalizedQuote> get(const std::string& exchange, 
                                                  const std::string& product) const {
        std::shared_lock lock(mutex_);  // Shared read lock (concurrent readers allowed)
        
        // Construct lookup key
        std::string key = make_key(exchange, product);
        
        // Search the map (O(log N) binary search in red-black tree)
        auto it = cache_.find(key);
        if (it == cache_.end()) return std::nullopt;  // Not found
        
        return it->second;  // Return the quote wrapped in std::optional
    }
    
    /**
     * get_all_for_product: Get quotes from all exchanges for a specific product
     * 
     * USE CASE: Cross-exchange arbitrage
     * - "Show me BTC-USD prices on all exchanges"
     * - Compare prices to find arbitrage opportunities
     * 
     * ALGORITHM:
     * 1. Acquire shared read lock
     * 2. Iterate through all quotes in cache
     * 3. Filter by product_id (ignoring exchange)
     * 4. Collect matching quotes in vector
     * 5. Release lock and return
     * 
     * COMPLEXITY:
     * - O(N) where N = total quotes in cache
     * - Typical: N = 50-100 (5 exchanges × 10-20 products)
     * - Execution time: ~1-5 microseconds
     * 
     * EXAMPLE RETURN:
     * product = "BTC-USD"
     * Returns: [
     *   {exchange: "coinbase", bid: 50000, ask: 50010, ...},
     *   {exchange: "binance", bid: 50005, ask: 50015, ...},
     *   {exchange: "kraken", bid: 49995, ask: 50005, ...}
     * ]
     * 
     * @param product: Product ID to filter by
     * @return Vector of all quotes for this product across exchanges
     */
    std::vector<pipeline::NormalizedQuote> get_all_for_product(const std::string& product) const {
        std::shared_lock lock(mutex_);  // Shared read lock
        std::vector<pipeline::NormalizedQuote> results;
        
        // Iterate through all cached quotes
        for (const auto& [key, quote] : cache_) {
            if (quote.product_id == product) {  // Match on product, ignore exchange
                results.push_back(quote);  // Copy quote to results
            }
        }
        
        return results;  // Return by value (move semantics apply)
    }
    
    /**
     * get_all_products: Get a list of all unique products in the cache
     * 
     * USE CASE: UI/Monitoring
     * - "What products are we tracking?"
     * - "Show me all available trading pairs"
     * 
     * ALGORITHM:
     * 1. Acquire shared read lock
     * 2. Iterate through all quotes
     * 3. Extract product_id from each quote
     * 4. Sort the list
     * 5. Remove duplicates using std::unique
     * 6. Return unique products
     * 
     * DUPLICATE REMOVAL:
     * - Same product appears on multiple exchanges
     * - Example: "BTC-USD" on Coinbase, Binance, Kraken
     * - We want just one "BTC-USD" in the output
     * 
     * ALGORITHM DETAIL:
     * - std::sort: Orders products alphabetically
     * - std::unique: Moves duplicates to end, returns new end iterator
     * - erase: Removes the duplicate tail
     * 
     * EXAMPLE:
     * Before: ["BTC-USD", "ETH-USD", "BTC-USD", "LTC-USD", "ETH-USD"]
     * After sort: ["BTC-USD", "BTC-USD", "ETH-USD", "ETH-USD", "LTC-USD"]
     * After unique: ["BTC-USD", "ETH-USD", "LTC-USD", ???, ???] (duplicates at end)
     * After erase: ["BTC-USD", "ETH-USD", "LTC-USD"]
     * 
     * @return Vector of unique product IDs, sorted alphabetically
     */
    std::vector<std::string> get_all_products() const {
        std::shared_lock lock(mutex_);  // Shared read lock
        std::vector<std::string> products;
        
        // Extract product_id from each quote
        for (const auto& [key, quote] : cache_) {
            products.push_back(quote.product_id);
        }
        
        // Sort alphabetically (required for std::unique to work)
        std::sort(products.begin(), products.end());
        
        // Remove duplicates:
        // - std::unique moves duplicates to end, returns iterator to new end
        // - erase removes everything from new end to old end
        products.erase(std::unique(products.begin(), products.end()), products.end());
        
        return products;
    }
    
    /**
     * size: Get the total number of cached quotes
     * 
     * NOTE: This is NOT the number of unique products
     * - size() returns total quotes (all exchange-product pairs)
     * - Example: 3 exchanges × 10 products = 30
     * 
     * @return Number of (exchange, product) pairs cached
     */
    size_t size() const {
        std::shared_lock lock(mutex_);  // Shared read lock
        return cache_.size();
    }
    
    /**
     * update_count: Get total number of updates since start
     * 
     * USE CASE: Monitoring
     * - "How many quote updates have we processed?"
     * - Rate calculation: updates per second
     * 
     * THREAD SAFETY:
     * - Atomic load, no lock needed
     * - Can read concurrently with writes
     * 
     * @return Total update count (monotonically increasing)
     */
    uint64_t update_count() const { return update_count_.load(); }

private:
    /**
     * make_key: Construct unique key from exchange and product
     * 
     * FORMAT: "exchange:product_id"
     * 
     * EXAMPLES:
     * - "coinbase:BTC-USD"
     * - "binance:ETH-USDT"
     * - "kraken:XBT-USD" (Kraken uses XBT for Bitcoin)
     * 
     * WHY COLON SEPARATOR?
     * - Simple, readable
     * - Colon doesn't appear in exchange names or product IDs
     * - Easy to split for debugging: key.split(':')
     * 
     * @param exchange: Exchange name
     * @param product: Product ID
     * @return Unique key string
     */
    std::string make_key(const std::string& exchange, const std::string& product) const {
        return exchange + ":" + product;
    }
    
    /**
     * MEMBER VARIABLES:
     * 
     * mutex_: Reader-writer lock
     * - mutable: Allows locking in const methods (get, size, etc.)
     * - shared_mutex: Optimized for many readers, few writers
     * 
     * cache_: Main storage
     * - std::map: Ordered container (red-black tree)
     * - Key: "exchange:product_id"
     * - Value: NormalizedQuote
     * - O(log N) lookup, but N is small
     * 
     * update_count_: Statistics counter
     * - Atomic: Thread-safe without lock
     * - Monotonically increasing
     * - Used for monitoring throughput
     */
    mutable std::shared_mutex mutex_;  // Reader-writer lock
    std::map<std::string, pipeline::NormalizedQuote> cache_;  // Ordered quote storage
    std::atomic<uint64_t> update_count_{0};  // Total updates counter
};

/**
 * COMPARISON: QuoteCache vs LatestQuotesCache
 * 
 * QuoteCache (this file):
 * - std::map (ordered)
 * - Returns std::optional
 * - Provides get_all_for_product()
 * - Provides get_all_products()
 * - Better for cross-exchange queries
 * 
 * LatestQuotesCache:
 * - std::unordered_map (unordered)
 * - Returns bool + output parameter
 * - Provides get_exchange_quotes()
 * - Provides per-exchange statistics
 * - Slightly faster lookup (O(1) vs O(log N))
 * 
 * WHEN TO USE WHICH?
 * - Use QuoteCache: When you need product aggregation
 * - Use LatestQuotesCache: When you need per-exchange stats
 * - Both are thread-safe and fast enough for HFT
 */

} // namespace storage
