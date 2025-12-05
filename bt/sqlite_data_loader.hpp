// ============================================================================
// bt/sqlite_data_loader.hpp - Load Historical Data from SQLite for Backtesting
// ============================================================================
//
// This module loads real historical market data from SQLite database
// for backtesting. NO simulated data - only real recorded data.
//
// Usage:
//   SQLiteDataLoader loader("demo_market_data.db");
//   auto ticks = loader.load_quotes("BTC-USD", start_time, end_time);
//   for (const auto& tick : ticks) {
//       // Process tick...
//   }
//
// ============================================================================

#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <sqlite3.h>

namespace bt {

// ============================================================================
// Data Structures
// ============================================================================

struct HistoricalQuote {
    int64_t timestamp_us;   // Unix microseconds
    std::string exchange;
    std::string product_id;
    double best_bid;
    double best_ask;
    double bid_size;
    double ask_size;
    int64_t sequence;
    int64_t latency_us;
    
    double mid() const { return (best_bid + best_ask) / 2.0; }
    double spread() const { return best_ask - best_bid; }
    double spread_bps() const { 
        double m = mid();
        return m > 0 ? (spread() / m) * 10000.0 : 0.0;
    }
};

struct HistoricalTrade {
    int64_t timestamp_us;
    std::string exchange;
    std::string product_id;
    double price;
    double size;
    std::string side;  // "buy" or "sell"
    std::string trade_id;
    int64_t sequence;
};

struct HistoricalOHLCV {
    int64_t timestamp_us;
    std::string exchange;
    std::string product_id;
    int interval_sec;
    double open;
    double high;
    double low;
    double close;
    double volume;
    int num_trades;
};

// ============================================================================
// SQLiteDataLoader Class
// ============================================================================

class SQLiteDataLoader {
public:
    explicit SQLiteDataLoader(const std::string& db_path) : db_path_(db_path), db_(nullptr) {
        int rc = sqlite3_open_v2(db_path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("Cannot open database: " + err);
        }
        std::cout << "[SQLiteDataLoader] ✓ Opened database: " << db_path << "\n";
    }
    
    ~SQLiteDataLoader() {
        if (db_) {
            sqlite3_close(db_);
        }
    }
    
    // Disable copy
    SQLiteDataLoader(const SQLiteDataLoader&) = delete;
    SQLiteDataLoader& operator=(const SQLiteDataLoader&) = delete;
    
    // ========================================================================
    // Load Quotes
    // ========================================================================
    
    /**
     * Load all quotes for a product
     */
    std::vector<HistoricalQuote> load_all_quotes(const std::string& product_id) {
        return load_quotes(product_id, "", 0, INT64_MAX);
    }
    
    /**
     * Load quotes for a product within a time range
     * @param product_id Product to load
     * @param exchange Exchange filter (empty = all)
     * @param start_time_us Start timestamp (Unix microseconds)
     * @param end_time_us End timestamp (Unix microseconds)
     */
    std::vector<HistoricalQuote> load_quotes(
        const std::string& product_id,
        const std::string& exchange = "",
        int64_t start_time_us = 0,
        int64_t end_time_us = INT64_MAX
    ) {
        std::vector<HistoricalQuote> quotes;
        
        std::string sql = 
            "SELECT timestamp, exchange, product_id, best_bid, best_ask, "
            "       bid_size, ask_size, sequence, latency_us "
            "FROM quotes WHERE product_id = ?";
        
        if (!exchange.empty()) {
            sql += " AND exchange = ?";
        }
        sql += " AND timestamp >= ? AND timestamp <= ?";
        sql += " ORDER BY timestamp ASC";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare statement: " + std::string(sqlite3_errmsg(db_)));
        }
        
        int idx = 1;
        sqlite3_bind_text(stmt, idx++, product_id.c_str(), -1, SQLITE_STATIC);
        if (!exchange.empty()) {
            sqlite3_bind_text(stmt, idx++, exchange.c_str(), -1, SQLITE_STATIC);
        }
        sqlite3_bind_int64(stmt, idx++, start_time_us);
        sqlite3_bind_int64(stmt, idx++, end_time_us);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            HistoricalQuote q;
            q.timestamp_us = sqlite3_column_int64(stmt, 0);
            q.exchange = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            q.product_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            q.best_bid = sqlite3_column_double(stmt, 3);
            q.best_ask = sqlite3_column_double(stmt, 4);
            q.bid_size = sqlite3_column_double(stmt, 5);
            q.ask_size = sqlite3_column_double(stmt, 6);
            q.sequence = sqlite3_column_int64(stmt, 7);
            q.latency_us = sqlite3_column_int64(stmt, 8);
            quotes.push_back(q);
        }
        
        sqlite3_finalize(stmt);
        std::cout << "[SQLiteDataLoader] Loaded " << quotes.size() << " quotes for " << product_id << "\n";
        return quotes;
    }
    
    /**
     * Load quotes for multiple products (for arbitrage backtesting)
     */
    std::vector<HistoricalQuote> load_quotes_multi(
        const std::vector<std::string>& product_ids,
        int64_t start_time_us = 0,
        int64_t end_time_us = INT64_MAX
    ) {
        std::vector<HistoricalQuote> all_quotes;
        for (const auto& pid : product_ids) {
            auto quotes = load_quotes(pid, "", start_time_us, end_time_us);
            all_quotes.insert(all_quotes.end(), quotes.begin(), quotes.end());
        }
        
        // Sort by timestamp
        std::sort(all_quotes.begin(), all_quotes.end(), 
            [](const HistoricalQuote& a, const HistoricalQuote& b) {
                return a.timestamp_us < b.timestamp_us;
            });
        
        return all_quotes;
    }
    
    // ========================================================================
    // Load Trades
    // ========================================================================
    
    /**
     * Load trades for a product
     */
    std::vector<HistoricalTrade> load_trades(
        const std::string& product_id,
        const std::string& exchange = "",
        int64_t start_time_us = 0,
        int64_t end_time_us = INT64_MAX
    ) {
        std::vector<HistoricalTrade> trades;
        
        std::string sql = 
            "SELECT timestamp, exchange, product_id, price, size, side, trade_id, sequence "
            "FROM trades WHERE product_id = ?";
        
        if (!exchange.empty()) {
            sql += " AND exchange = ?";
        }
        sql += " AND timestamp >= ? AND timestamp <= ?";
        sql += " ORDER BY timestamp ASC";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare statement: " + std::string(sqlite3_errmsg(db_)));
        }
        
        int idx = 1;
        sqlite3_bind_text(stmt, idx++, product_id.c_str(), -1, SQLITE_STATIC);
        if (!exchange.empty()) {
            sqlite3_bind_text(stmt, idx++, exchange.c_str(), -1, SQLITE_STATIC);
        }
        sqlite3_bind_int64(stmt, idx++, start_time_us);
        sqlite3_bind_int64(stmt, idx++, end_time_us);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            HistoricalTrade t;
            t.timestamp_us = sqlite3_column_int64(stmt, 0);
            t.exchange = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            t.product_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            t.price = sqlite3_column_double(stmt, 3);
            t.size = sqlite3_column_double(stmt, 4);
            t.side = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            const char* tid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            t.trade_id = tid ? tid : "";
            t.sequence = sqlite3_column_int64(stmt, 7);
            trades.push_back(t);
        }
        
        sqlite3_finalize(stmt);
        std::cout << "[SQLiteDataLoader] Loaded " << trades.size() << " trades for " << product_id << "\n";
        return trades;
    }
    
    // ========================================================================
    // Load OHLCV
    // ========================================================================
    
    std::vector<HistoricalOHLCV> load_ohlcv(
        const std::string& product_id,
        int interval_sec = 60,
        int64_t start_time_us = 0,
        int64_t end_time_us = INT64_MAX
    ) {
        std::vector<HistoricalOHLCV> bars;
        
        std::string sql = 
            "SELECT timestamp, exchange, product_id, interval_sec, "
            "       open, high, low, close, volume, num_trades "
            "FROM ohlcv WHERE product_id = ? AND interval_sec = ?";
        sql += " AND timestamp >= ? AND timestamp <= ?";
        sql += " ORDER BY timestamp ASC";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare statement: " + std::string(sqlite3_errmsg(db_)));
        }
        
        sqlite3_bind_text(stmt, 1, product_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, interval_sec);
        sqlite3_bind_int64(stmt, 3, start_time_us);
        sqlite3_bind_int64(stmt, 4, end_time_us);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            HistoricalOHLCV bar;
            bar.timestamp_us = sqlite3_column_int64(stmt, 0);
            bar.exchange = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            bar.product_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            bar.interval_sec = sqlite3_column_int(stmt, 3);
            bar.open = sqlite3_column_double(stmt, 4);
            bar.high = sqlite3_column_double(stmt, 5);
            bar.low = sqlite3_column_double(stmt, 6);
            bar.close = sqlite3_column_double(stmt, 7);
            bar.volume = sqlite3_column_double(stmt, 8);
            bar.num_trades = sqlite3_column_int(stmt, 9);
            bars.push_back(bar);
        }
        
        sqlite3_finalize(stmt);
        std::cout << "[SQLiteDataLoader] Loaded " << bars.size() << " OHLCV bars for " << product_id << "\n";
        return bars;
    }
    
    // ========================================================================
    // Database Statistics
    // ========================================================================
    
    struct DataStats {
        int64_t total_quotes = 0;
        int64_t total_trades = 0;
        int64_t total_ohlcv = 0;
        int64_t min_timestamp = 0;
        int64_t max_timestamp = 0;
        std::vector<std::string> products;
        std::vector<std::string> exchanges;
    };
    
    DataStats get_stats() {
        DataStats stats;
        
        // Count quotes
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM quotes", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.total_quotes = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        
        // Count trades
        sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM trades", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.total_trades = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        
        // Count OHLCV
        sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM ohlcv", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.total_ohlcv = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        
        // Time range
        sqlite3_prepare_v2(db_, "SELECT MIN(timestamp), MAX(timestamp) FROM quotes", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.min_timestamp = sqlite3_column_int64(stmt, 0);
            stats.max_timestamp = sqlite3_column_int64(stmt, 1);
        }
        sqlite3_finalize(stmt);
        
        // Products
        sqlite3_prepare_v2(db_, "SELECT DISTINCT product_id FROM quotes", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.products.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);
        
        // Exchanges
        sqlite3_prepare_v2(db_, "SELECT DISTINCT exchange FROM quotes", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.exchanges.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);
        
        return stats;
    }
    
    void print_stats() {
        auto stats = get_stats();
        std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║              SQLite Database Statistics                       ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ Total Quotes:  " << stats.total_quotes << "\n";
        std::cout << "║ Total Trades:  " << stats.total_trades << "\n";
        std::cout << "║ Total OHLCV:   " << stats.total_ohlcv << "\n";
        std::cout << "║ Time Range:    " << stats.min_timestamp << " - " << stats.max_timestamp << "\n";
        std::cout << "║ Products:      ";
        for (const auto& p : stats.products) std::cout << p << " ";
        std::cout << "\n";
        std::cout << "║ Exchanges:     ";
        for (const auto& e : stats.exchanges) std::cout << e << " ";
        std::cout << "\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    }
    
private:
    std::string db_path_;
    sqlite3* db_;
};

} // namespace bt
