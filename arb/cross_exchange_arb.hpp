// arb/cross_exchange_arb.hpp - Cross-Exchange Arbitrage Detection
#pragma once

#include "arbitrage_opportunity.hpp"     // ArbOpportunity struct
#include "../exchanges/exchange_config.hpp" // Exchange fee configuration
#include <map>        // For nested map storage
#include <vector>     // For opportunity lists
#include <algorithm>  // For std::sort
#include <chrono>     // For timestamps

namespace arb {

/**
 * ExchangeQuote: Quote data from a specific exchange
 * 
 * PURPOSE:
 * - Store orderbook top-of-book data from one exchange
 * - Used for cross-exchange price comparison
 * 
 * FIELDS:
 * - exchange: Exchange name (e.g., "coinbase", "binance")
 * - bid/ask: Best prices on this exchange
 * - bid_size/ask_size: Quantities available
 * - timestamp: When this quote was received (for staleness detection)
 */
struct ExchangeQuote {
    std::string exchange;  // Exchange identifier
    double bid;            // Best bid price (highest buy offer)
    double ask;            // Best ask price (lowest sell offer)
    double bid_size;       // Quantity available at best bid
    double ask_size;       // Quantity available at best ask
    std::chrono::system_clock::time_point timestamp;  // Quote timestamp
};

/**
 * CrossExchangeArbitrage: Detect price discrepancies across exchanges
 * 
 * THEORY:
 * - Same asset should have same price on all exchanges (Law of One Price)
 * - In practice, prices differ due to:
 *   1. Different liquidity pools
 *   2. Network latency
 *   3. Market segmentation
 *   4. Exchange-specific events
 * 
 * ARBITRAGE OPPORTUNITY:
 * - Buy on Exchange A at price P_A
 * - Sell on Exchange B at price P_B
 * - Profit: (P_B - P_A) - fees
 * 
 * CHALLENGES:
 * - Transfer time: Moving BTC between exchanges takes 10-60 minutes
 * - Withdrawal fees: $20-50 per transfer
 * - Counterparty risk: One exchange might freeze withdrawals
 * - Capital requirements: Need funds on both exchanges
 * 
 * PRACTICAL APPROACH (Statistical Arbitrage):
 * - Don't actually transfer assets
 * - Hold inventory on both exchanges
 * - When Binance > Coinbase: Sell on Binance, buy on Coinbase
 * - When Coinbase > Binance: Sell on Coinbase, buy on Binance
 * - Periodically rebalance inventory
 * 
 * BACKTEST RESULTS (typical):
 * - Opportunities: 5-20 per day (for BTC)
 * - Duration: 0.5-5 seconds
 * - Net profit after fees: 5-30 bps per opportunity
 */
class CrossExchangeArbitrage {
public:
    /**
     * Constructor
     * 
     * @param min_profit_bps: Minimum net profit in basis points (default 15)
     *                        1 bp = 0.01% = 0.0001
     *                        15 bps = 0.15% = $150 profit on $100k trade
     * 
     * WHY 15 BPS?
     * - Typical taker fees: 10-20 bps per side = 20-40 bps total
     * - Need profit > fees to be worthwhile
     * - 15 bps net = ~35-55 bps gross spread
     * - This filters out noise and ensures quality signals
     */
    CrossExchangeArbitrage(double min_profit_bps = 15.0)
        : min_profit_bps_(min_profit_bps) {}

    /**
     * update_quote: Store a quote from an exchange
     * 
     * ALGORITHM:
     * - Store quotes in nested map: product → exchange → quote
     * - Latest quote overwrites previous quote
     * - No staleness detection here (caller's responsibility)
     * 
     * DATA STRUCTURE:
     * quotes_["BTC-USD"]["coinbase"] = ExchangeQuote{...}
     * quotes_["BTC-USD"]["binance"] = ExchangeQuote{...}
     * quotes_["BTC-USD"]["kraken"] = ExchangeQuote{...}
     * 
     * @param product: Trading pair (e.g., "BTC-USD")
     * @param quote: Quote data from the exchange
     */
    void update_quote(const std::string& product, const ExchangeQuote& quote) {
        // Store: product → exchange → quote
        quotes_[product][quote.exchange] = quote;
    }

    /**
     * find_opportunities: Find all arbitrage opportunities for a product
     * 
     * ALGORITHM:
     * 1. Get all exchange quotes for this product
     * 2. Compare every pair of exchanges (N choose 2 combinations)
     * 3. For each pair, check both directions:
     *    - Buy on A, sell on B
     *    - Buy on B, sell on A
     * 4. Calculate gross spread (price difference)
     * 5. Subtract fees to get net spread
     * 6. If net spread > threshold, record opportunity
     * 7. Sort by profitability (highest first)
     * 
     * COMPLEXITY:
     * - N exchanges: N*(N-1) comparisons
     * - 3 exchanges: 6 comparisons
     * - 5 exchanges: 20 comparisons
     * - Still O(N^2) but N is small (typically 2-5)
     * 
     * @param product: Trading pair to analyze
     * @return Vector of opportunities, sorted by net profit (descending)
     */
    std::vector<ArbOpportunity> find_opportunities(const std::string& product) {
        std::vector<ArbOpportunity> opportunities;
        
        // Look up all quotes for this product
        auto it = quotes_.find(product);
        if (it == quotes_.end() || it->second.size() < 2) {
            return opportunities;  // Need at least 2 exchanges to arbitrage
        }
        
        const auto& exchange_quotes = it->second;
        
        /**
         * NESTED LOOP: Check all exchange pairs
         * 
         * WHY TWO LOOPS?
         * - Need to compare every exchange with every other exchange
         * - Use iterators to avoid duplicate comparisons
         * 
         * EXAMPLE:
         * Exchanges: [Coinbase, Binance, Kraken]
         * 
         * Comparisons:
         * - Coinbase vs Binance (both directions)
         * - Coinbase vs Kraken (both directions)
         * - Binance vs Kraken (both directions)
         * 
         * Total: 3 * 2 = 6 checks
         */
        for (auto it1 = exchange_quotes.begin(); it1 != exchange_quotes.end(); ++it1) {
            for (auto it2 = std::next(it1); it2 != exchange_quotes.end(); ++it2) {
                // Check: Buy on exchange1, sell on exchange2
                check_arb(product, it1->second, it2->second, opportunities);
                
                // Check: Buy on exchange2, sell on exchange1
                check_arb(product, it2->second, it1->second, opportunities);
            }
        }
        
        /**
         * SORT BY PROFITABILITY:
         * - Most profitable opportunities first
         * - Execute highest profit trades if capital is limited
         * - Lambda comparator: returns true if a > b
         */
        std::sort(opportunities.begin(), opportunities.end(),
                 [](const ArbOpportunity& a, const ArbOpportunity& b) {
                     return a.net_spread_bps > b.net_spread_bps;
                 });
        
        return opportunities;
    }

private:
    /**
     * check_arb: Check if buying on one exchange and selling on another is profitable
     * 
     * ALGORITHM:
     * 1. Extract buy price (ask on buy exchange) and sell price (bid on sell exchange)
     * 2. Check if sell price > buy price (otherwise, not profitable)
     * 3. Calculate gross spread in basis points
     * 4. Look up exchange fees
     * 5. Calculate net spread = gross - fees
     * 6. Check if net spread > minimum threshold
     * 7. Calculate maximum quantity (limited by orderbook depth)
     * 8. Calculate expected profit in USD
     * 9. Create and store opportunity object
     * 
     * WHY CHECK BOTH DIRECTIONS?
     * - Bid-ask spreads are different on each exchange
     * - Coinbase bid might be higher than Binance ask (arb exists)
     * - Binance bid might be higher than Coinbase ask (reverse arb)
     * 
     * @param product: Trading pair
     * @param buy_q: Quote from exchange where we buy
     * @param sell_q: Quote from exchange where we sell
     * @param opportunities: Output vector where opportunities are appended
     */
    void check_arb(const std::string& product,
                   const ExchangeQuote& buy_q,
                   const ExchangeQuote& sell_q,
                   std::vector<ArbOpportunity>& opportunities) {
        /**
         * PRICE EXTRACTION:
         * - To buy: We pay the ask price (seller's price)
         * - To sell: We receive the bid price (buyer's price)
         * 
         * EXAMPLE:
         * Coinbase: bid=$50,000, ask=$50,010
         * Binance:  bid=$50,020, ask=$50,030
         * 
         * Opportunity: Buy on Coinbase at $50,010, sell on Binance at $50,020
         * Gross profit: $10 per BTC
         */
        double buy_price = buy_q.ask;   // Price we pay to buy
        double sell_price = sell_q.bid; // Price we receive when selling
        
        // Sanity check: Can't arbitrage if sell price <= buy price
        if (sell_price <= buy_price) return;
        
        /**
         * GROSS SPREAD CALCULATION:
         * 
         * Formula: ((sell - buy) / buy) * 10,000
         * 
         * EXAMPLE:
         * buy = $50,000, sell = $50,100
         * gross_bps = ((50,100 - 50,000) / 50,000) * 10,000
         *           = (100 / 50,000) * 10,000
         *           = 0.002 * 10,000
         *           = 20 bps
         */
        double gross_bps = ((sell_price - buy_price) / buy_price) * 10000.0;
        
        /**
         * FEE LOOKUP:
         * - Each exchange has different fee structure
         * - Taker fees (aggressive orders): 0.1-0.2% typically
         * - Maker fees (passive orders): 0.0-0.1% typically
         * - We assume taker fees (worst case, immediate execution)
         */
        auto buy_cfg = exchanges::EXCHANGES.find(buy_q.exchange);
        auto sell_cfg = exchanges::EXCHANGES.find(sell_q.exchange);
        
        // Skip if exchange config not found
        if (buy_cfg == exchanges::EXCHANGES.end() || 
            sell_cfg == exchanges::EXCHANGES.end()) return;
        
        /**
         * NET SPREAD CALCULATION:
         * 
         * fee_bps = (buy_fee% + sell_fee%) * 100
         * net_bps = gross_bps - fee_bps
         * 
         * EXAMPLE:
         * Coinbase taker fee: 0.10% = 10 bps
         * Binance taker fee: 0.10% = 10 bps
         * Total fees: 20 bps
         * 
         * gross_bps = 30 bps
         * net_bps = 30 - 20 = 10 bps
         */
        double fee_bps = (buy_cfg->second.taker_fee_pct + sell_cfg->second.taker_fee_pct) * 100.0;
        double net_bps = gross_bps - fee_bps;
        
        // Filter: Only record if net profit exceeds threshold
        if (net_bps < min_profit_bps_) return;
        
        /**
         * QUANTITY CALCULATION:
         * - Limited by available liquidity on both sides
         * - Can only trade min(buy_size, sell_size)
         * 
         * EXAMPLE:
         * Coinbase ask: 0.5 BTC available
         * Binance bid: 2.0 BTC available
         * Max quantity: 0.5 BTC (limited by Coinbase)
         */
        double qty = std::min(buy_q.ask_size, sell_q.bid_size);
        
        /**
         * PROFIT CALCULATION:
         * 
         * gross_profit = (sell_price - buy_price) * qty
         * fees = buy_notional * buy_fee + sell_notional * sell_fee
         * net_profit = gross_profit - fees
         * 
         * SIMPLIFIED (assuming equal fees):
         * net_profit = price_diff * qty - total_notional * total_fee_pct
         * 
         * EXAMPLE:
         * buy_price = $50,000, sell_price = $50,100
         * qty = 0.5 BTC
         * gross = $100 * 0.5 = $50
         * fees = ($50,000 * 0.5) * 0.002 = $50
         * net = $50 - $50 = $0 (break-even after fees!)
         */
        double profit = (sell_price - buy_price) * qty - 
                       (buy_price * qty * fee_bps / 10000.0);
        
        /**
         * CREATE OPPORTUNITY OBJECT:
         * - Populate all fields for execution and monitoring
         * - confidence: 0.8 (arbitrary, could be improved with ML)
         * - executable: true (assumes sufficient capital and API access)
         */
        ArbOpportunity opp;
        opp.type = ArbType::CROSS_EXCHANGE;  // Distinguish from triangular, etc.
        opp.buy_exchange = buy_q.exchange;   // Where to buy
        opp.sell_exchange = sell_q.exchange; // Where to sell
        opp.product = product;               // Trading pair
        opp.buy_price = buy_price;           // Execution prices
        opp.sell_price = sell_price;
        opp.gross_spread_bps = gross_bps;    // Before fees
        opp.net_spread_bps = net_bps;        // After fees (what we actually earn)
        opp.quantity = qty;                  // Max tradeable quantity
        opp.expected_profit_usd = profit;    // Expected profit in dollars
        opp.confidence = 0.8;                // Confidence score (could use ML here)
        opp.executable = true;               // Assume we can execute (needs validation)
        opp.timestamp = std::chrono::system_clock::now();  // When detected
        
        // Add to output vector
        opportunities.push_back(opp);
    }

    double min_profit_bps_;  // Minimum net profit threshold (e.g., 15 bps)
    
    /**
     * QUOTE STORAGE:
     * 
     * Data structure: Nested map
     * - Outer map: product → inner map
     * - Inner map: exchange → quote
     * 
     * EXAMPLE:
     * quotes_["BTC-USD"]["coinbase"] = ExchangeQuote{bid=50000, ask=50010, ...}
     * quotes_["BTC-USD"]["binance"]  = ExchangeQuote{bid=50020, ask=50030, ...}
     * quotes_["ETH-USD"]["coinbase"] = ExchangeQuote{bid=3000, ask=3002, ...}
     * 
     * LOOKUP COMPLEXITY:
     * - O(1) for product lookup (std::map is O(log N) but N is small)
     * - O(1) for exchange lookup
     * - Total: O(log N) where N = number of products
     */
    std::map<std::string, std::map<std::string, ExchangeQuote>> quotes_;
};

/**
 * EXECUTION CONSIDERATIONS:
 * 
 * RISK FACTORS:
 * 1. Quote staleness: Prices might have changed since quote received
 * 2. Partial fills: Might not get full quantity
 * 3. Slippage: Actual price worse than quoted price
 * 4. Exchange downtime: One exchange might reject order
 * 5. Withdrawal freezes: Can't rebalance inventory
 * 
 * MITIGATION:
 * - Timestamp checking: Reject stale quotes (> 100ms old)
 * - Position limits: Don't over-allocate to one exchange
 * - Diversification: Spread across multiple exchanges
 * - Reserve capital: Keep buffer for unexpected events
 * 
 * CAPITAL REQUIREMENTS:
 * - Need inventory on both exchanges
 * - Example: $100k on Coinbase, $100k on Binance
 * - As you execute arbs, inventory shifts
 * - Periodically rebalance (manual or automated transfers)
 */

} // namespace arb
