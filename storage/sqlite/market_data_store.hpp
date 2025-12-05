// storage/sqlite/market_data_store.hpp - Persistent Market Data Storage with SQLite
#pragma once

#include "../../pipeline/normalized_data.hpp" // NormalizedQuote structure
#include <sqlite3.h>  // SQLite C API
#include <string>     // For file paths
#include <mutex>      // For thread safety
#include <memory>     // For smart pointers
#include <chrono>     // For timestamp conversion
#include <iostream>   // For logging

namespace storage {

/**
 * MarketDataStore: Persistent storage for market data using SQLite
 * 
 * PURPOSE:
 * - Historical data for backtesting
 * - Regulatory compliance (audit trail)
 * - Data analysis and research
 * - Crash recovery (WAL mode)
 * 
 * WHY SQLite?
 * - Embedded database (no separate server process)
 * - ACID transactions (atomic, consistent, isolated, durable)
 * - Fast for write-heavy workloads with proper tuning
 * - Single file (easy backup/transfer)
 * - Zero configuration
 * 
 * PERFORMANCE OPTIMIZATIONS:
 * - WAL mode: 2-10x faster writes than default rollback journal
 * - Prepared statements: Avoid SQL parsing overhead
 * - Batch transactions: Group inserts for ~100x speedup
 * - Large cache: 64MB for hot data
 * - Memory temp storage: Fast for sorting/aggregation
 * 
 * TYPICAL THROUGHPUT:
 * - Single insert: ~1ms (with WAL, synchronous=NORMAL)
 * - Batched (1000 inserts): ~10ms total = 0.01ms per insert
 * - Batch mode: ~100,000 inserts/sec
 * 
 * DESIGN TRADE-OFFS:
 * - Not lock-free (uses mutex)
 * - Not real-time (async writes on separate thread recommended)
 * - Not distributed (single machine only)
 * - But: Simple, reliable, and fast enough for most HFT use cases
 */
class MarketDataStore {
public:
    /**
     * Constructor: Open database and configure for performance
     * 
     * INITIALIZATION STEPS:
     * 1. Open SQLite database file (creates if doesn't exist)
     * 2. Set performance PRAGMAs
     * 3. Create tables and indexes
     * 4. Prepare SQL statements for reuse
     * 
     * PRAGMA SETTINGS EXPLAINED:
     * 
     * journal_mode = WAL (Write-Ahead Logging):
     * - Default: Rollback journal (slower)
     * - WAL: Writes go to separate log file, later merged
     * - Benefit: 2-10x faster writes, readers don't block writers
     * - Tradeoff: Slightly more complex, requires cleanup
     * 
     * synchronous = NORMAL:
     * - FULL: fsync() after every write (~100 writes/sec)
     * - NORMAL: fsync() less frequently (~1000 writes/sec)
     * - OFF: Never fsync (fastest but risky on crash)
     * - Choice: NORMAL balances speed and durability
     * 
     * cache_size = -64000:
     * - Negative value: KB of memory (64MB = 64000KB)
     * - Positive value: Pages (default 2000 pages = ~8MB)
     * - 64MB: Holds ~100K quotes in memory
     * - Benefit: Faster queries, less disk I/O
     * 
     * temp_store = MEMORY:
     * - Temporary tables used for ORDER BY, GROUP BY
     * - Default: Disk (slow)
     * - MEMORY: Use RAM (fast)
     * 
     * @param db_path: Path to SQLite database file
     * @throws std::runtime_error if database cannot be opened
     */
    explicit MarketDataStore(const std::string& db_path = "hft_market_data.db") {
        // Open database (creates file if doesn't exist)
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to open database: " + std::string(sqlite3_errmsg(db_)));
        }
        
        /**
         * PERFORMANCE PRAGMAS:
         * 
         * Each PRAGMA is a database configuration setting
         * These are critical for HFT performance
         */
        
        // WAL mode: 2-10x faster writes, readers don't block writers
        exec("PRAGMA journal_mode = WAL");
        
        // NORMAL sync: Balance between speed and durability
        exec("PRAGMA synchronous = NORMAL");
        
        // 64MB cache: Keep hot data in memory
        exec("PRAGMA cache_size = -64000");
        
        // Temp tables in memory: Faster sorting/aggregation
        exec("PRAGMA temp_store = MEMORY");
        
        // Create schema (tables and indexes)
        create_tables();
        
        // Prepare reusable SQL statements
        prepare_statements();
        
        std::cout << "[STORAGE] Opened database: " << db_path << "\n";
    }
    
    /**
     * Destructor: Clean up resources
     * 
     * CLEANUP ORDER:
     * 1. Finalize prepared statements (releases resources)
     * 2. Close database connection
     * 
     * NOTE: SQLite auto-commits any pending transaction on close
     */
    ~MarketDataStore() {
        finalize_statements();  // Release prepared statements
        if (db_) sqlite3_close(db_);  // Close database
    }
    
    /**
     * insert_quote: Store a single quote (fast path using prepared statement)
     * 
     * ALGORITHM:
     * 1. Lock mutex (thread safety)
     * 2. Convert timestamp to microseconds since epoch
     * 3. Reset prepared statement
     * 4. Bind parameters (avoid SQL injection)
     * 5. Execute statement
     * 6. Release lock
     * 
     * WHY PREPARED STATEMENTS?
     * - SQL is parsed once, reused many times
     * - Parsing overhead: ~100μs
     * - With prepared: ~1ms total insert time
     * - Without prepared: ~1.1ms total insert time
     * - 10% faster (more significant at high volumes)
     * 
     * PARAMETER BINDING:
     * - Index starts at 1 (not 0!)
     * - SQLITE_TRANSIENT: SQLite copies the string
     * - Alternative: SQLITE_STATIC (caller guarantees lifetime)
     * 
     * THREAD SAFETY:
     * - SQLite is NOT thread-safe by default
     * - We use std::mutex to serialize access
     * - Alternative: Compile SQLite with SQLITE_THREADSAFE=2
     * 
     * @param quote: The quote to store
     * @return true if insert succeeded, false otherwise
     */
    bool insert_quote(const pipeline::NormalizedQuote& quote) {
        std::lock_guard<std::mutex> lock(mutex_);  // Thread safety
        
        /**
         * TIMESTAMP CONVERSION:
         * - C++ chrono: time_point (complex type)
         * - SQLite: INTEGER (microseconds since epoch)
         * - Why microseconds? Milliseconds lose too much precision
         */
        int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
            quote.local_timestamp.time_since_epoch()).count();
        
        /**
         * PREPARED STATEMENT EXECUTION:
         * 
         * Step 1: Reset statement (clears previous bindings)
         * Step 2-9: Bind parameters (fills in the ? placeholders)
         * Step 10: Execute (sqlite3_step)
         */
        
        // Reset the statement (clears bindings from previous use)
        sqlite3_reset(stmt_insert_quote_);
        
        // Bind parameters (index starts at 1)
        sqlite3_bind_text(stmt_insert_quote_, 1, quote.exchange.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_insert_quote_, 2, quote.product_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_insert_quote_, 3, ts);
        sqlite3_bind_double(stmt_insert_quote_, 4, quote.best_bid);
        sqlite3_bind_double(stmt_insert_quote_, 5, quote.best_ask);
        sqlite3_bind_double(stmt_insert_quote_, 6, quote.bid_size);
        sqlite3_bind_double(stmt_insert_quote_, 7, quote.ask_size);
        sqlite3_bind_int64(stmt_insert_quote_, 8, quote.sequence);
        sqlite3_bind_int64(stmt_insert_quote_, 9, quote.latency_us());
        
        // Execute the INSERT statement
        int rc = sqlite3_step(stmt_insert_quote_);
        return rc == SQLITE_DONE;  // SQLITE_DONE = success
    }
    
    /**
     * begin_transaction: Start a batch transaction
     * 
     * WHY BATCH TRANSACTIONS?
     * - Default: Auto-commit after each INSERT (slow)
     * - Batched: One commit after N inserts (fast)
     * 
     * PERFORMANCE IMPACT:
     * - Without batching: 100 inserts = 100ms (1ms each)
     * - With batching: 100 inserts = 2ms total (0.02ms each)
     * - 50x faster!
     * 
     * USAGE PATTERN:
     * ```
     * store.begin_transaction();
     * for (auto& quote : quotes) {
     *     store.insert_quote(quote);
     * }
     * store.commit_transaction();
     * ```
     * 
     * ACID GUARANTEES:
     * - All inserts succeed, or all fail (atomicity)
     * - Database is in consistent state (consistency)
     * - Other readers see old data until commit (isolation)
     * - Changes persist after commit (durability)
     */
    void begin_transaction() {
        std::lock_guard<std::mutex> lock(mutex_);
        exec("BEGIN TRANSACTION");
    }
    
    /**
     * commit_transaction: Commit a batch transaction
     * 
     * WHAT HAPPENS ON COMMIT:
     * 1. All changes written to WAL file
     * 2. Checkpoint (WAL merged into main database)
     * 3. fsync() called (data reaches disk)
     * 
     * FAILURE SCENARIOS:
     * - If commit fails: Transaction is rolled back
     * - If crash before commit: Transaction is lost
     * - If crash after commit: Changes are durable
     */
    void commit_transaction() {
        std::lock_guard<std::mutex> lock(mutex_);
        exec("COMMIT");
    }
    
    /**
     * LatestQuote: Result structure for queries
     * 
     * DESIGN CHOICE:
     * - Could return NormalizedQuote, but that's heavier
     * - This is minimal: just bid, ask, timestamp
     * - found flag: Avoids exceptions for missing data
     */
    struct LatestQuote {
        double bid, ask;        // Best bid and ask prices
        int64_t timestamp;      // Timestamp in microseconds
        bool found;             // Was a quote found?
    };
    
    /**
     * get_latest_quote: Query the most recent quote for a product
     * 
     * ALGORITHM:
     * 1. Construct SQL query with ORDER BY timestamp DESC
     * 2. Prepare statement
     * 3. Bind parameters (exchange, product)
     * 4. Execute and fetch result
     * 5. Finalize statement
     * 6. Return result
     * 
     * SQL EXPLANATION:
     * - WHERE: Filter to specific exchange and product
     * - ORDER BY timestamp DESC: Newest first
     * - LIMIT 1: Return only one row
     * 
     * INDEX USAGE:
     * - idx_quotes_exchange_product_time covers this query
     * - Index scan instead of full table scan
     * - Fast: O(log N) instead of O(N)
     * 
     * WHY NOT USE PREPARED STATEMENT?
     * - This is a read query (less frequent than writes)
     * - Preparing once and reusing would be better for hot path
     * - But for simplicity, we prepare on-demand
     * 
     * @param exchange: Exchange name
     * @param product: Product ID
     * @return LatestQuote struct (found=false if no data)
     */
    LatestQuote get_latest_quote(const std::string& exchange, const std::string& product) {
        std::lock_guard<std::mutex> lock(mutex_);  // Thread safety
        
        // Construct SQL query
        std::string sql = "SELECT best_bid, best_ask, timestamp FROM quotes "
                         "WHERE exchange = ? AND product_id = ? "
                         "ORDER BY timestamp DESC LIMIT 1";
        
        // Prepare statement
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        
        // Bind parameters
        sqlite3_bind_text(stmt, 1, exchange.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, product.c_str(), -1, SQLITE_TRANSIENT);
        
        // Execute and fetch result
        LatestQuote result{0, 0, 0, false};
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            // Extract column values (index starts at 0 for results)
            result.bid = sqlite3_column_double(stmt, 0);
            result.ask = sqlite3_column_double(stmt, 1);
            result.timestamp = sqlite3_column_int64(stmt, 2);
            result.found = true;
        }
        
        // Clean up
        sqlite3_finalize(stmt);
        return result;
    }
    
    /**
     * get_quote_count: Get total number of quotes in database
     * 
     * USE CASE: Monitoring, statistics
     * 
     * SQL: COUNT(*) without WHERE clause
     * - Scans entire table (can be slow for large tables)
     * - SQLite optimizes this for some cases
     * - Alternative: Track count in separate metadata table
     * 
     * @return Total quote count
     */
    int64_t get_quote_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM quotes", -1, &stmt, nullptr);
        
        int64_t count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        
        sqlite3_finalize(stmt);
        return count;
    }

private:
    /**
     * create_tables: Create schema (tables and indexes)
     * 
     * SCHEMA DESIGN:
     * 
     * quotes table:
     * - id: Auto-incrementing primary key (for row ordering)
     * - exchange, product_id: Identify the quote
     * - timestamp: When the quote was received (microseconds)
     * - best_bid, best_ask: Top of book prices
     * - bid_size, ask_size: Quantities at best bid/ask
     * - sequence: Exchange sequence number (gap detection)
     * - latency_us: Network latency in microseconds
     * 
     * INDEX DESIGN:
     * - idx_quotes_exchange_product_time: Composite index
     * - Covers: WHERE exchange = ? AND product_id = ? ORDER BY timestamp
     * - Without index: Full table scan O(N)
     * - With index: Binary search O(log N)
     * 
     * WHY NOT MORE INDEXES?
     * - Each index adds overhead on INSERT
     * - Index maintenance: 20-50% slower inserts
     * - Balance: Enough indexes for common queries, not too many
     */
    void create_tables() {
        const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS quotes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    exchange TEXT NOT NULL,
    product_id TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    best_bid REAL NOT NULL,
    best_ask REAL NOT NULL,
    bid_size REAL NOT NULL,
    ask_size REAL NOT NULL,
    sequence INTEGER,
    latency_us INTEGER
);
CREATE INDEX IF NOT EXISTS idx_quotes_exchange_product_time ON quotes(exchange, product_id, timestamp);
        )SQL";
        
        exec(schema);
    }
    
    /**
     * prepare_statements: Prepare frequently-used SQL statements
     * 
     * PREPARED STATEMENT LIFECYCLE:
     * 1. Prepare: Parse SQL, create execution plan
     * 2. Bind: Fill in parameter values
     * 3. Step: Execute statement
     * 4. Reset: Clear bindings, ready for reuse
     * 5. Finalize: Release resources
     * 
     * WHY PREPARE ONCE?
     * - Parsing SQL is expensive (~100μs)
     * - Prepared statement reused thousands of times
     * - Amortized cost: <1ns per use
     * 
     * STATEMENT: INSERT INTO quotes
     * - 9 parameters (9 question marks)
     * - Binding happens in insert_quote()
     * - Reset after each use (in insert_quote())
     * - Finalized in destructor
     */
    void prepare_statements() {
        const char* sql_insert_quote = 
            "INSERT INTO quotes (exchange, product_id, timestamp, best_bid, best_ask, "
            "bid_size, ask_size, sequence, latency_us) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
        
        // Prepare the statement (parse SQL, create execution plan)
        sqlite3_prepare_v2(db_, sql_insert_quote, -1, &stmt_insert_quote_, nullptr);
    }
    
    /**
     * finalize_statements: Release prepared statements
     * 
     * RESOURCE CLEANUP:
     * - Each prepared statement holds memory and locks
     * - Finalize releases these resources
     * - Called in destructor
     */
    void finalize_statements() {
        if (stmt_insert_quote_) sqlite3_finalize(stmt_insert_quote_);
    }
    
    /**
     * exec: Execute a simple SQL statement (no parameters)
     * 
     * USE CASES:
     * - PRAGMA settings
     * - CREATE TABLE
     * - BEGIN/COMMIT
     * 
     * ERROR HANDLING:
     * - Throws exception on error
     * - Alternative: Return bool (caller checks)
     * 
     * @param sql: SQL statement to execute
     * @throws std::runtime_error on SQL error
     */
    void exec(const char* sql) {
        char* err_msg = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string error = err_msg ? err_msg : "Unknown error";
            sqlite3_free(err_msg);  // Free error message
            throw std::runtime_error("SQL error: " + error);
        }
    }
    
    /**
     * MEMBER VARIABLES:
     * 
     * db_: SQLite database handle
     * - Opaque pointer to internal SQLite structures
     * - All SQLite operations require this handle
     * 
     * stmt_insert_quote_: Prepared INSERT statement
     * - Reused for every insert_quote() call
     * - Avoids SQL parsing overhead
     * 
     * mutex_: Thread safety lock
     * - SQLite is not thread-safe by default
     * - We serialize all access with this mutex
     * - Alternative: Use SQLite in serialized mode
     */
    sqlite3* db_;                   // Database connection handle
    sqlite3_stmt* stmt_insert_quote_;  // Prepared INSERT statement
    std::mutex mutex_;              // Thread safety lock
};

/**
 * USAGE PATTERNS:
 * 
 * 1. SINGLE INSERT (simple, slower):
 * ```
 * MarketDataStore store("data.db");
 * store.insert_quote(quote);  // Auto-commit, ~1ms
 * ```
 * 
 * 2. BATCH INSERT (recommended, faster):
 * ```
 * MarketDataStore store("data.db");
 * store.begin_transaction();
 * for (auto& quote : quotes) {
 *     store.insert_quote(quote);  // ~0.02ms each
 * }
 * store.commit_transaction();  // One commit for all
 * ```
 * 
 * 3. QUERY:
 * ```
 * auto latest = store.get_latest_quote("coinbase", "BTC-USD");
 * if (latest.found) {
 *     std::cout << "Bid: " << latest.bid << ", Ask: " << latest.ask << "\n";
 * }
 * ```
 * 
 * PRODUCTION RECOMMENDATIONS:
 * - Run storage on separate thread (don't block hot path)
 * - Use MPMC queue to feed storage thread
 * - Batch inserts every 100-1000 quotes
 * - Monitor database size (can grow to GB)
 * - Periodic vacuum to reclaim space
 * - Consider time-based partitioning for large datasets
 */

} // namespace storage
