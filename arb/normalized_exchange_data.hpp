#pragma once

//==============================================================================
// NORMALIZED MULTI-EXCHANGE DATA STRUCTURES
//==============================================================================
// Extends existing pipeline structures for multi-exchange funding rate arbitrage
// Reuses: core/order_book.hpp, pipeline/normalized_data.hpp, pipeline/spsc_queue.hpp

#include "../core/order_book.hpp"
#include "../pipeline/normalized_data.hpp"
#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <optional>
#include <algorithm>

namespace arb {

//==============================================================================
// EXCHANGE IDENTIFIER (8 bytes - cache-friendly)
//==============================================================================

enum class ExchangeID : uint8_t {
    BINANCE  = 0,
    BYBIT    = 1,
    OKX      = 2,
    GATEIO   = 3,
    MEXC     = 4,
    KUCOIN   = 5,
    KRAKEN   = 6,
    BITGET   = 7,
    HTX      = 8,
    BINGX    = 9,
    DERIBIT  = 10,
    COINBASE = 11,
    UNKNOWN  = 255
};

// Fast string-to-enum conversion (O(1) hash lookup)
inline ExchangeID exchange_from_string(const std::string& s) {
    static const std::map<std::string, ExchangeID> lookup = {
        {"binance", ExchangeID::BINANCE}, {"bybit", ExchangeID::BYBIT},
        {"okx", ExchangeID::OKX}, {"gateio", ExchangeID::GATEIO},
        {"mexc", ExchangeID::MEXC}, {"kucoin", ExchangeID::KUCOIN},
        {"kraken", ExchangeID::KRAKEN}, {"bitget", ExchangeID::BITGET},
        {"htx", ExchangeID::HTX}, {"bingx", ExchangeID::BINGX},
        {"deribit", ExchangeID::DERIBIT}, {"coinbase", ExchangeID::COINBASE}
    };
    auto it = lookup.find(s);
    return (it != lookup.end()) ? it->second : ExchangeID::UNKNOWN;
}

inline const char* exchange_to_string(ExchangeID id) {
    static const char* names[] = {
        "binance", "bybit", "okx", "gateio", "mexc", "kucoin",
        "kraken", "bitget", "htx", "bingx", "deribit", "coinbase", "unknown"
    };
    return names[static_cast<uint8_t>(id)];
}

//==============================================================================
// UNIFIED SYMBOL (Normalized across exchanges)
//==============================================================================

struct UnifiedSymbol {
    std::string base;   // "BTC", "ETH", etc
    std::string quote;  // "USDT", "USD", etc
    
    std::string to_string() const { return base + "/" + quote; }
    
    bool operator==(const UnifiedSymbol& other) const {
        return base == other.base && quote == other.quote;
    }
    
    bool operator<(const UnifiedSymbol& other) const {
        return base < other.base || (base == other.base && quote < other.quote);
    }
    
    // Parse exchange-specific format to unified format
    static UnifiedSymbol normalize(const std::string& exchange_symbol) {
        std::string norm = exchange_symbol;
        std::transform(norm.begin(), norm.end(), norm.begin(), ::toupper);
        
        // Remove common suffixes
        const std::vector<std::string> suffixes = {
            "USDT", "USDC", "USD", "PERP", "-PERP", "_PERP",
            "-SWAP", "_SWAP", "SWAP", "-UMCBL", "_UMCBL",
            "BUSD", "TUSD", "_", "-"
        };
        
        std::string quote = "USD";  // default
        for (const auto& suffix : suffixes) {
            size_t pos = norm.find(suffix);
            if (pos != std::string::npos) {
                if (suffix.find("USDT") != std::string::npos) quote = "USDT";
                else if (suffix.find("USDC") != std::string::npos) quote = "USDC";
                else if (suffix.find("USD") != std::string::npos) quote = "USD";
                norm = norm.substr(0, pos);
                break;
            }
        }
        
        return UnifiedSymbol{norm, quote};
    }
};

//==============================================================================
// NORMALIZED ORDERBOOK SNAPSHOT (Compatible with existing core::OrderBook)
//==============================================================================

struct NormalizedOrderbookSnapshot {
    // Identity
    ExchangeID exchange_id;
    std::string exchange_symbol;  // Original exchange symbol "BTCUSDT"
    UnifiedSymbol unified_symbol; // Normalized "BTC/USDT"
    
    // Timestamps (monotonic + system time)
    int64_t exchange_timestamp_ns;  // Exchange's timestamp in nanoseconds
    int64_t local_timestamp_ns;     // Our receive timestamp
    uint64_t sequence;              // Exchange sequence number
    
    // Best bid/ask (hot path - cache-aligned)
    double best_bid_price;
    double best_bid_qty;
    double best_ask_price;
    double best_ask_qty;
    
    // Full orderbook depth (up to N levels)
    std::vector<std::pair<double, double>> bids;  // Sorted descending
    std::vector<std::pair<double, double>> asks;  // Sorted ascending
    
    // Derived metrics (cached for hot path)
    double mid_price() const { return (best_bid_price + best_ask_price) / 2.0; }
    double spread_bps() const { 
        double mid = mid_price();
        return mid > 0 ? ((best_ask_price - best_bid_price) / mid) * 10000.0 : 0.0;
    }
    double liquidity_imbalance() const {
        double total = best_bid_qty + best_ask_qty;
        return total > 0 ? (best_bid_qty - best_ask_qty) / total : 0.0;
    }
    int64_t latency_ns() const { return local_timestamp_ns - exchange_timestamp_ns; }
    
    // Convert to existing core::OrderBook format
    core::OrderBook to_orderbook() const {
        core::OrderBook ob;
        for (const auto& [price, qty] : bids) {
            ob.set_level(true, price, qty);
        }
        for (const auto& [price, qty] : asks) {
            ob.set_level(false, price, qty);
        }
        return ob;
    }
    
    // Convert from existing pipeline::NormalizedQuote
    static NormalizedOrderbookSnapshot from_quote(
        const pipeline::NormalizedQuote& quote) {
        
        NormalizedOrderbookSnapshot snap;
        snap.exchange_id = exchange_from_string(quote.exchange);
        snap.exchange_symbol = quote.product_id;
        snap.unified_symbol = UnifiedSymbol{quote.base, quote.quote};
        
        snap.exchange_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            quote.exchange_timestamp.time_since_epoch()).count();
        snap.local_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            quote.local_timestamp.time_since_epoch()).count();
        snap.sequence = quote.sequence;
        
        snap.best_bid_price = quote.best_bid;
        snap.best_bid_qty = quote.bid_size;
        snap.best_ask_price = quote.best_ask;
        snap.best_ask_qty = quote.ask_size;
        
        snap.bids = quote.bids;
        snap.asks = quote.asks;
        
        return snap;
    }
} __attribute__((aligned(64)));  // Cache-line aligned

//==============================================================================
// FUNDING RATE DATA (Extended for arbitrage)
//==============================================================================

struct FundingRateSnapshot {
    // Identity
    ExchangeID exchange_id;
    std::string exchange_symbol;
    UnifiedSymbol unified_symbol;
    
    // Timestamps
    int64_t timestamp_ns;
    int64_t next_funding_time_ns;
    
    // Funding rate info
    double funding_rate;           // Per-interval rate (e.g., 0.0001 = 0.01%)
    double funding_rate_annual;    // Annualized %
    int funding_interval_hours;    // Payment interval (1, 4, 8 hours)
    
    // Mark and index prices
    double mark_price;
    double index_price;
    
    // Derived metrics
    double hours_until_funding() const {
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return static_cast<double>(next_funding_time_ns - now_ns) / (1e9 * 3600.0);
    }
    
    double payments_per_year() const {
        return (24.0 / funding_interval_hours) * 365.0;
    }
} __attribute__((aligned(64)));

//==============================================================================
// COMBINED MARKET DATA (Orderbook + Funding)
//==============================================================================

struct UnifiedMarketData {
    NormalizedOrderbookSnapshot orderbook;
    std::optional<FundingRateSnapshot> funding;
    
    bool has_funding() const { return funding.has_value(); }
    bool is_valid() const {
        return orderbook.best_bid_price > 0 && 
               orderbook.best_ask_price > 0 &&
               orderbook.best_bid_price < orderbook.best_ask_price;
    }
} __attribute__((aligned(64)));

//==============================================================================
// CROSS-EXCHANGE ARBITRAGE OPPORTUNITY
//==============================================================================

struct ArbitrageOpportunity {
    // Opportunity type
    enum class Type : uint8_t {
        FUNDING_RATE,      // Funding rate differential
        PRICE_ARBITRAGE,   // Simple price difference
        TRIANGULAR,        // Three-way arbitrage
        STATISTICAL        // Stat arb mean reversion
    } type;
    
    // Unified symbol
    UnifiedSymbol symbol;
    
    // Long leg (where we go long)
    ExchangeID long_exchange;
    std::string long_symbol;
    double long_price;
    double long_funding_rate_annual;
    
    // Short leg (where we go short)
    ExchangeID short_exchange;
    std::string short_symbol;
    double short_price;
    double short_funding_rate_annual;
    
    // Opportunity metrics
    double spread_annual;         // APY spread %
    double price_difference_bps;  // Price difference in bps
    double net_profit_apy;        // After fees
    
    // Risk metrics
    double correlation;           // Price correlation (1.0 = perfect)
    double liquidity_score;       // Min liquidity on both sides
    
    // Metadata
    int64_t detected_timestamp_ns;
    int num_exchanges_with_symbol;  // How many exchanges list this
    
    // Profitability check
    bool is_profitable(double min_apy = 10.0) const {
        return net_profit_apy >= min_apy;
    }
} __attribute__((aligned(64)));

//==============================================================================
// EXCHANGE FEE STRUCTURE
//==============================================================================

struct ExchangeFees {
    double maker_fee;  // Limit order fee
    double taker_fee;  // Market order fee
    
    static ExchangeFees get(ExchangeID exchange) {
        static const ExchangeFees fees[] = {
            {0.0002, 0.0004},  // BINANCE
            {0.0002, 0.00055}, // BYBIT
            {0.0002, 0.0005},  // OKX
            {0.0002, 0.0005},  // GATEIO
            {0.0000, 0.0006},  // MEXC (maker 0%)
            {0.0002, 0.0006},  // KUCOIN
            {0.0002, 0.0005},  // KRAKEN
            {0.0002, 0.0006},  // BITGET
            {0.0002, 0.0005},  // HTX
            {0.0002, 0.0005},  // BINGX
            {0.0000, 0.0005},  // DERIBIT (maker 0%)
            {0.0050, 0.0050},  // COINBASE (retail)
            {0.0004, 0.0006}   // UNKNOWN (conservative)
        };
        return fees[static_cast<uint8_t>(exchange)];
    }
};

} // namespace arb
