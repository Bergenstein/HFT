//==============================================================================
// MARKET NEUTRAL PAIRS TRADING STRATEGY
//==============================================================================
//
// THEORY & RATIONALE:
// -------------------
// Pairs trading is a market-neutral strategy that exploits mean reversion in
// the relative prices of two correlated assets. By going long one asset and
// short another, we eliminate market risk and profit from their convergence.
//
// KEY CONCEPT: Statistical Arbitrage
// - Two assets with high correlation (e.g., BTC and ETH, or BTC on two exchanges)
// - Calculate spread: spread(t) = price_A(t) - β * price_B(t)
// - When spread deviates from mean, bet on convergence
// - Enter: Long underpriced asset, short overpriced asset
// - Exit: When spread returns to mean
//
// MATHEMATICAL FOUNDATION:
// ------------------------
// 1. Cointegration Test (Engle-Granger):
//    - Two price series P_A and P_B are cointegrated if:
//      P_A(t) = α + β * P_B(t) + ε(t)
//    - Where ε(t) is stationary (mean-reverting)
//
// 2. Hedge Ratio (β):
//    - β = Cov(P_A, P_B) / Var(P_B)
//    - Or: β from linear regression P_A ~ β * P_B
//    - Ensures dollar-neutral position
//
// 3. Spread Calculation:
//    spread(t) = P_A(t) - β * P_B(t)
//
// 4. Z-score (Entry Signal):
//    z(t) = (spread(t) - μ_spread) / σ_spread
//
// 5. Entry Conditions:
//    - LONG A, SHORT B if z(t) < -z_entry (spread too low)
//    - SHORT A, LONG B if z(t) > +z_entry (spread too high)
//    - Typical z_entry = 2.0 (2 standard deviations)
//
// 6. Exit Conditions:
//    - Close when z(t) crosses zero (mean reversion)
//    - Or stop-loss at z(t) = ±z_stop (default 3.5)
//
// 7. Position Sizing (Dollar Neutral):
//    - If LONG A, SHORT B:
//      * Quantity_A = Position_Size / P_A
//      * Quantity_B = (β * Position_Size) / P_B
//    - This ensures: Value_A ≈ β * Value_B
//
// EXAMPLE:
// --------
// Assets: BTC (Coinbase) vs BTC (Binance)
//
// Historical Data (30 days):
//   - β = 0.998 (nearly 1:1, but slight bias)
//   - μ_spread = $5.00 (Coinbase usually $5 higher)
//   - σ_spread = $15.00
//
// Current Prices:
//   - Coinbase BTC: $50,000
//   - Binance BTC: $49,950
//   - spread = 50000 - 0.998 * 49950 = 50000 - 49850 = $150
//   - z-score = (150 - 5) / 15 = 145 / 15 = 9.67
//
// Signal: z = 9.67 > 2.0 → Spread too high
// Action: SHORT Coinbase, LONG Binance
//
// Position Sizing ($100k total):
//   - SHORT 1.0 BTC on Coinbase ($50,000)
//   - LONG 1.002 BTC on Binance (0.998 * $50,000 = $49,900, so ~1.002 BTC)
//
// Exit:
//   - Wait for z-score to cross 0
//   - If spread returns to $5: profit = $145 per BTC = $145 on $100k = 0.145%
//   - Minus fees (0.2% round trip) = -0.055% net loss
//
// This shows why we need larger z-scores or better execution!
//
// WHEN TO USE:
// ------------
// 1. High correlation pairs (ρ > 0.90):
//    - BTC vs ETH (β ≈ 0.05, i.e., 1 BTC ≈ 20 ETH)
//    - Same asset different exchanges
//    - Spot vs futures (with funding adjustment)
//
// 2. Stationary spread:
//    - Augmented Dickey-Fuller test p-value < 0.05
//    - Visual inspection: spread oscillates around mean
//
// 3. Sufficient volatility:
//    - σ_spread large enough to overcome fees
//    - Need z-score swings of ±2 to be profitable
//
// WHY IT WORKS:
// -------------
// - Short-term supply/demand imbalances between venues
// - Different user bases (Coinbase retail, Binance institutional)
// - Network effects and liquidity fragmentation
// - Psychological anchoring to previous price ratios
// - Statistical regression to the mean (fundamental property)
//
// RISKS:
// ------
// 1. Cointegration breakdown: Relationship changes permanently
//    - Example: ETH 2.0 upgrade changes BTC/ETH dynamics
//
// 2. Divergence before convergence: Spread widens before narrowing
//    - Stop-loss at z = ±3.5 limits this
//
// 3. Execution risk: Can't enter both legs simultaneously
//    - Use limit orders and accept partial fills
//
// 4. Exchange risk: One exchange goes down mid-trade
//
// 5. Fat tails: Extreme events (z > 5) more common than normal distribution suggests
//
// 6. Model risk: β is estimated, not known
//    - Re-estimate periodically (e.g., every 30 days)
//
// ADVANCED TECHNIQUES:
// --------------------
// 1. Dynamic β: Update hedge ratio using Kalman filter
// 2. Multi-leg: Trade 3+ assets simultaneously (e.g., BTC, ETH, LTC)
// 3. Intraday patterns: Entry signals stronger at certain times
// 4. Volume weighting: Weight recent data more heavily
// 5. Distance method: Use spread distance instead of z-score
//
// TYPICAL PARAMETERS:
// -------------------
// - Lookback window: 30-90 days for β estimation
// - Entry z-score: ±2.0 to ±2.5
// - Exit z-score: 0 (mean reversion)
// - Stop-loss z-score: ±3.5 to ±4.0
// - Min correlation: 0.85
// - Position size: 20-40% of portfolio per pair
// - Re-estimation frequency: Weekly
//
//==============================================================================

#pragma once

#include "arbitrage_opportunity.hpp"
#include <vector>
#include <deque>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <string>

namespace arb {

/**
 * PairData: Price data for one asset in a pair
 */
struct PairData {
    std::string exchange;
    std::string product;
    double price;           // Mid price or last trade
    double bid;
    double ask;
    double bid_size;
    double ask_size;
    std::chrono::system_clock::time_point timestamp;
};

/**
 * PairStatistics: Statistical properties of a trading pair
 */
struct PairStatistics {
    double beta;            // Hedge ratio
    double mean_spread;     // Historical mean spread
    double std_spread;      // Standard deviation of spread
    double correlation;     // Pearson correlation coefficient
    double current_spread;  // Current spread value
    double z_score;         // Current z-score
    size_t sample_size;     // Number of historical samples
};

/**
 * MarketNeutralPairs: Market-neutral pairs trading strategy
 */
class MarketNeutralPairs {
public:
    /**
     * Constructor
     * 
     * @param entry_z_score: Z-score threshold to enter trade (default 2.0)
     * @param exit_z_score: Z-score to exit trade (default 0.5)
     * @param stop_loss_z_score: Z-score for stop-loss (default 3.5)
     * @param lookback_window: Number of samples for statistics (default 720 = 30 days @ 1/hr)
     * @param min_correlation: Minimum correlation to trade pair (default 0.85)
     */
    MarketNeutralPairs(double entry_z_score = 2.0,
                       double exit_z_score = 0.5,
                       double stop_loss_z_score = 3.5,
                       size_t lookback_window = 720,
                       double min_correlation = 0.85)
        : entry_z_score_(entry_z_score),
          exit_z_score_(exit_z_score),
          stop_loss_z_score_(stop_loss_z_score),
          lookback_window_(lookback_window),
          min_correlation_(min_correlation) {}

    /**
     * update_price: Store price data for statistical calculation
     * 
     * @param asset_a: Price data for first asset
     * @param asset_b: Price data for second asset
     */
    void update_price(const PairData& asset_a, const PairData& asset_b) {
        std::string pair_key = make_pair_key(asset_a, asset_b);
        
        auto& history = price_history_[pair_key];
        history.first.push_back(asset_a.price);
        history.second.push_back(asset_b.price);
        
        // Keep only recent history
        if (history.first.size() > lookback_window_) {
            history.first.pop_front();
            history.second.pop_front();
        }
        
        // Store current data
        current_prices_[pair_key] = std::make_pair(asset_a, asset_b);
    }
    
    /**
     * calculate_statistics: Compute β, mean, std dev, correlation, z-score
     * 
     * ALGORITHM:
     * 1. Extract price series for both assets
     * 2. Calculate β using linear regression
     * 3. Calculate spread = P_A - β * P_B for all points
     * 4. Calculate mean and std dev of spread
     * 5. Calculate Pearson correlation
     * 6. Calculate current z-score
     * 
     * @param asset_a: First asset identifier
     * @param asset_b: Second asset identifier
     * @return Statistics if sufficient data exists
     */
    std::optional<PairStatistics> calculate_statistics(
        const PairData& asset_a,
        const PairData& asset_b) {
        
        std::string pair_key = make_pair_key(asset_a, asset_b);
        
        auto it = price_history_.find(pair_key);
        if (it == price_history_.end() || it->second.first.size() < 30) {
            return std::nullopt;  // Need at least 30 samples
        }
        
        const auto& prices_a = it->second.first;
        const auto& prices_b = it->second.second;
        const size_t n = prices_a.size();
        
        // Calculate means
        double mean_a = std::accumulate(prices_a.begin(), prices_a.end(), 0.0) / n;
        double mean_b = std::accumulate(prices_b.begin(), prices_b.end(), 0.0) / n;
        
        // Calculate covariance and variance
        double cov_ab = 0.0;
        double var_a = 0.0;
        double var_b = 0.0;
        
        for (size_t i = 0; i < n; ++i) {
            double dev_a = prices_a[i] - mean_a;
            double dev_b = prices_b[i] - mean_b;
            cov_ab += dev_a * dev_b;
            var_a += dev_a * dev_a;
            var_b += dev_b * dev_b;
        }
        
        cov_ab /= n;
        var_a /= n;
        var_b /= n;
        
        // Calculate β (hedge ratio)
        double beta = (var_b > 0) ? (cov_ab / var_b) : 1.0;
        
        // Calculate correlation
        double correlation = (var_a > 0 && var_b > 0) 
            ? (cov_ab / std::sqrt(var_a * var_b)) 
            : 0.0;
        
        // Check minimum correlation
        if (std::abs(correlation) < min_correlation_) {
            return std::nullopt;
        }
        
        // Calculate spread series
        std::vector<double> spreads;
        spreads.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            spreads.push_back(prices_a[i] - beta * prices_b[i]);
        }
        
        // Calculate spread statistics
        double mean_spread = std::accumulate(spreads.begin(), spreads.end(), 0.0) / n;
        
        double sum_sq = 0.0;
        for (double s : spreads) {
            sum_sq += (s - mean_spread) * (s - mean_spread);
        }
        double std_spread = std::sqrt(sum_sq / n);
        
        // Calculate current spread
        double current_spread = asset_a.price - beta * asset_b.price;
        
        // Calculate z-score
        double z_score = (std_spread > 0) 
            ? (current_spread - mean_spread) / std_spread 
            : 0.0;
        
        PairStatistics stats;
        stats.beta = beta;
        stats.mean_spread = mean_spread;
        stats.std_spread = std_spread;
        stats.correlation = correlation;
        stats.current_spread = current_spread;
        stats.z_score = z_score;
        stats.sample_size = n;
        
        return stats;
    }
    
    /**
     * find_opportunity: Look for pairs trading opportunity
     * 
     * ALGORITHM:
     * 1. Calculate current statistics
     * 2. Check if z-score exceeds entry threshold
     * 3. Determine trade direction (long spread or short spread)
     * 4. Calculate position sizes for dollar neutrality
     * 5. Estimate expected profit from mean reversion
     * 
     * @param asset_a: First asset data
     * @param asset_b: Second asset data
     * @return ArbOpportunity if found
     */
    std::optional<ArbOpportunity> find_opportunity(
        const PairData& asset_a,
        const PairData& asset_b) {
        
        auto stats = calculate_statistics(asset_a, asset_b);
        if (!stats) {
            return std::nullopt;
        }
        
        // Check if z-score is extreme enough
        if (std::abs(stats->z_score) < entry_z_score_) {
            return std::nullopt;
        }
        
        ArbOpportunity opp;
        opp.type = ArbType::STATISTICAL;
        opp.product = asset_a.product;  // Primary asset
        opp.timestamp = asset_a.timestamp;
        
        if (stats->z_score > 0) {
            // Spread too high: Asset A overpriced relative to B
            // Action: SHORT A, LONG B
            opp.sell_exchange = asset_a.exchange;
            opp.buy_exchange = asset_b.exchange;
            opp.sell_price = asset_a.bid;  // Sell A at bid
            opp.buy_price = asset_b.ask;   // Buy B at ask
            
            // Dollar-neutral sizing
            // If we short $100k of A, we need to long β * $100k of B
            // Store quantity as units of asset A (will need to adjust B separately)
            opp.quantity = 1.0;  // Normalized
            
        } else {
            // Spread too low: Asset A underpriced relative to B
            // Action: LONG A, SHORT B
            opp.buy_exchange = asset_a.exchange;
            opp.sell_exchange = asset_b.exchange;
            opp.buy_price = asset_a.ask;   // Buy A at ask
            opp.sell_price = asset_b.bid;  // Sell B at bid
            
            opp.quantity = 1.0;
        }
        
        // Expected profit: spread will revert from current to mean
        // Profit = |z_score| * std_spread (in dollars per unit)
        const double spread_reversion = std::abs(stats->z_score) * stats->std_spread;
        
        // Convert to basis points relative to asset A price
        opp.gross_spread_bps = (spread_reversion / asset_a.price) * 10000.0;
        
        // Fees: need to trade both legs
        // Typical: 0.10% per side = 0.20% total per leg = 0.40% round trip
        const double fee_bps = 40.0;
        opp.net_spread_bps = opp.gross_spread_bps - fee_bps;
        
        // Expected profit in dollars
        // Assuming $100k position in asset A
        const double position_value = 100000.0;
        opp.expected_profit_usd = (opp.net_spread_bps / 10000.0) * position_value;
        
        // Confidence based on:
        // 1. Z-score magnitude (higher = more certain)
        // 2. Sample size (more data = more confident)
        // 3. Correlation strength (higher = more reliable)
        const double z_conf = std::min(std::abs(stats->z_score) / 4.0, 1.0) * 0.40;
        const double sample_conf = std::min(static_cast<double>(stats->sample_size) / lookback_window_, 1.0) * 0.30;
        const double corr_conf = std::min((std::abs(stats->correlation) - min_correlation_) / 
                                          (1.0 - min_correlation_), 1.0) * 0.30;
        
        opp.confidence = std::min(z_conf + sample_conf + corr_conf, 1.0);
        
        return (opp.net_spread_bps > 0) ? std::optional<ArbOpportunity>(opp) : std::nullopt;
    }
    
    /**
     * should_exit: Check if existing pairs position should be closed
     * 
     * Exit conditions:
     * 1. Z-score reverted to near zero (target achieved)
     * 2. Z-score expanded beyond stop-loss
     * 3. Correlation dropped below threshold
     * 
     * @param asset_a: Current data for asset A
     * @param asset_b: Current data for asset B
     * @param entry_z_score: Z-score when position was entered
     * @return true if should exit
     */
    bool should_exit(const PairData& asset_a,
                     const PairData& asset_b,
                     double entry_z_score) {
        
        auto stats = calculate_statistics(asset_a, asset_b);
        if (!stats) {
            return true;  // Exit if we can't calculate stats
        }
        
        // Exit if z-score reverted to mean
        if (std::abs(stats->z_score) < exit_z_score_) {
            return true;
        }
        
        // Exit if z-score hit stop-loss
        if (std::abs(stats->z_score) > stop_loss_z_score_) {
            return true;
        }
        
        // Exit if correlation broke down
        if (std::abs(stats->correlation) < min_correlation_) {
            return true;
        }
        
        // Exit if z-score crossed zero (mean reversion completed)
        // Only check if entry and current have opposite signs
        if ((entry_z_score > 0 && stats->z_score < 0) ||
            (entry_z_score < 0 && stats->z_score > 0)) {
            return true;
        }
        
        return false;
    }
    
    /**
     * Get current statistics for a pair (for monitoring)
     */
    std::optional<PairStatistics> get_current_stats(
        const PairData& asset_a,
        const PairData& asset_b) {
        return calculate_statistics(asset_a, asset_b);
    }

private:
    /**
     * Make unique key for asset pair
     */
    std::string make_pair_key(const PairData& a, const PairData& b) const {
        return a.exchange + ":" + a.product + "|" + b.exchange + ":" + b.product;
    }
    
    double entry_z_score_;        // Entry threshold
    double exit_z_score_;         // Exit threshold
    double stop_loss_z_score_;    // Stop-loss threshold
    size_t lookback_window_;      // Historical window size
    double min_correlation_;      // Minimum correlation to trade
    
    // Historical prices: pair_key -> (prices_a, prices_b)
    std::map<std::string, std::pair<std::deque<double>, std::deque<double>>> price_history_;
    
    // Current prices: pair_key -> (asset_a, asset_b)
    std::map<std::string, std::pair<PairData, PairData>> current_prices_;
};

} // namespace arb
