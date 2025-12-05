// pipeline/normalized_data.hpp - Normalized Market Data Structures
#pragma once

#include <string>   // For exchange names, symbols
#include <chrono>   // For timestamps
#include <vector>   // For orderbook levels

namespace pipeline {

/**
 * NormalizedQuote: Unified representation of Level 2 (orderbook) data
 * 
 * PURPOSE:
 * - Different exchanges have different JSON schemas
 * - Normalizing to a common format simplifies strategy code
 * - Allows cross-exchange comparison and arbitrage detection
 * 
 * DESIGN DECISIONS:
 * - Uses std::string for exchange/symbol (not enums) for flexibility
 * - Stores both timestamps: exchange (for backtesting) and local (for latency)
 * - Includes full orderbook depth (bids/asks vectors) for advanced strategies
 * - Best bid/ask extracted for O(1) access (most common use case)
 */
struct NormalizedQuote {
    // Identity
    std::string exchange;     // Exchange name: "coinbase", "binance", "kraken"
    std::string product_id;   // Trading pair: "BTC-USD", "ETH-USDT"
    std::string base;         // Base currency: "BTC", "ETH"
    std::string quote;        // Quote currency: "USD", "USDT"
    
    // Top of book (Level 1 data)
    double best_bid;   // Highest price someone is willing to buy at
    double best_ask;   // Lowest price someone is willing to sell at
    double bid_size;   // Quantity available at best bid
    double ask_size;   // Quantity available at best ask
    
    // Metadata
    int64_t sequence;  // Sequence number from exchange (for gap detection)
    std::chrono::system_clock::time_point exchange_timestamp;  // Exchange's timestamp
    std::chrono::system_clock::time_point local_timestamp;     // When we received it
    
    // Full orderbook depth (Level 2 data)
    // Each pair is (price, quantity)
    std::vector<std::pair<double, double>> bids;  // Sorted descending by price
    std::vector<std::pair<double, double>> asks;  // Sorted ascending by price
    
    /**
     * mid_price: Calculate the mid-market price
     * 
     * DEFINITION: Average of best bid and best ask
     * USE: Fair value estimation, spread calculation
     * 
     * EXAMPLE: bid=100.00, ask=100.10 → mid=100.05
     */
    double mid_price() const { return (best_bid + best_ask) / 2.0; }
    
    /**
     * spread_bps: Calculate bid-ask spread in basis points
     * 
     * DEFINITION: ((ask - bid) / mid) * 10,000
     * 
     * WHY BASIS POINTS?
     * - Percentage would be: ((ask - bid) / mid) * 100 → ~0.05%
     * - Basis points: multiply by 10,000 → ~5 bps
     * - Easier to talk about "5 bps" than "0.05%"
     * 
     * TYPICAL VALUES:
     * - BTC on Coinbase: 1-5 bps (tight)
     * - Small-cap altcoins: 50-500 bps (wide)
     */
    double spread_bps() const { 
        double mid = mid_price();
        return mid > 0 ? ((best_ask - best_bid) / mid) * 10000.0 : 0.0;
    }
    
    /**
     * imbalance: Calculate order book imbalance
     * 
     * DEFINITION: (bid_size - ask_size) / (bid_size + ask_size)
     * 
     * INTERPRETATION:
     * - +1.0: All liquidity on bid side (buy pressure)
     * -  0.0: Balanced
     * - -1.0: All liquidity on ask side (sell pressure)
     * 
     * USE IN STRATEGIES:
     * - Imbalance > 0.6 → Likely price increase → BUY signal
     * - Imbalance < -0.6 → Likely price decrease → SELL signal
     */
    double imbalance() const {
        double total = bid_size + ask_size;
        return total > 0 ? (bid_size - ask_size) / total : 0.0;
    }
    
    /**
     * latency_us: Calculate network latency in microseconds
     * 
     * CALCULATION: local_timestamp - exchange_timestamp
     * 
     * COMPONENTS:
     * - Exchange processing time: ~50μs
     * - Network transmission: ~100-500μs (depends on distance)
     * - Kernel processing: ~10μs
     * - User-space processing: ~5μs
     * - Total: ~200-600μs typical
     * 
     * WHY THIS MATTERS:
     * - Latency > 1ms → Something is wrong (network congestion, CPU throttling)
     * - Latency spikes → Discard old quotes (stale data)
     */
    int64_t latency_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            local_timestamp - exchange_timestamp).count();
    }
};

/**
 * NormalizedTrade: Unified representation of trade (execution) data
 * 
 * PURPOSE:
 * - Track actual trades (not just quotes)
 * - Volume analysis, VWAP calculation
 * - Trade flow analysis (buy vs sell pressure)
 * 
 * TRADE VS QUOTE:
 * - Quote: Someone places a limit order (passive)
 * - Trade: Someone takes liquidity with a market order (aggressive)
 */
struct NormalizedTrade {
    std::string exchange;     // Exchange name
    std::string product_id;   // Trading pair
    std::string trade_id;     // Unique trade ID from exchange
    
    double price;  // Execution price
    double size;   // Quantity traded
    
    std::string side;  // "buy" or "sell" (from taker's perspective)
                      // "buy" = taker bought (aggressor was buyer)
                      // "sell" = taker sold (aggressor was seller)
    
    std::chrono::system_clock::time_point timestamp;  // When trade occurred
    int64_t sequence;  // Sequence number for ordering
};

/**
 * OHLCVBar: Candlestick / OHLCV bar for charting and analysis
 * 
 * PURPOSE:
 * - Aggregate trade data into time intervals
 * - Technical analysis (moving averages, RSI, MACD)
 * - Charting and visualization
 * 
 * AGGREGATION INTERVALS:
 * - 1s, 5s, 15s: Tick charts for scalping
 * - 1m, 5m, 15m: Intraday trading
 * - 1h, 4h, 1d: Position trading
 */
struct OHLCVBar {
    std::string exchange;     // Exchange name
    std::string product_id;   // Trading pair
    
    std::chrono::system_clock::time_point timestamp;  // Bar start time
    int64_t interval_seconds;  // Bar duration (60 = 1 minute, 300 = 5 minutes)
    
    // OHLC: Open, High, Low, Close
    double open;   // First trade price in the interval
    double high;   // Highest trade price in the interval
    double low;    // Lowest trade price in the interval
    double close;  // Last trade price in the interval
    
    // Volume metrics
    double volume;      // Total quantity traded
    int64_t num_trades; // Number of trades in this interval
    
    /**
     * VWAP: Volume-Weighted Average Price
     * 
     * CALCULATION: Σ(price * volume) / Σ(volume)
     * 
     * WHY VWAP > CLOSE?
     * - Close only uses last trade (might be small trade)
     * - VWAP weighs by volume (more representative)
     * 
     * USE CASES:
     * - Execution benchmarking (did we beat VWAP?)
     * - Institutional traders use as target price
     */
    double vwap;  // Volume-weighted average price
};

} // namespace pipeline
