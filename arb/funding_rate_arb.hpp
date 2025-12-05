//==============================================================================
// FUNDING RATE ARBITRAGE STRATEGY
//==============================================================================
//
// THEORY & RATIONALE:
// -------------------
// Cryptocurrency perpetual futures use a funding rate mechanism to keep the
// perp price anchored to spot. Different exchanges calculate and apply funding
// rates differently, creating arbitrage opportunities when:
//   1. Funding rates diverge across exchanges for same asset
//   2. Predicted funding differs from current funding
//   3. Funding rate changes near payment time
//
// ARBITRAGE TYPES:
// ----------------
// Type 1: Cross-Exchange Funding Arbitrage
//   - Binance BTC funding: +0.05% (pay to be long)
//   - Bybit BTC funding: -0.02% (receive to be long)
//   - Action: LONG on Bybit (collect), SHORT on Binance (avoid payment)
//   - Profit: 0.05% + 0.02% = 0.07% every 8 hours = 0.21%/day = 76% APY
//
// Type 2: Funding Rate Mean Reversion
//   - Historical funding: μ = 0.01%, σ = 0.03%
//   - Current funding: 0.15% (z-score = 4.67)
//   - Expectation: Rate will revert to mean
//   - Action: SHORT perp (bet funding will decrease)
//   - Profit: Basis convergence as funding normalizes
//
// Type 3: Predicted vs Actual Funding
//   - Predicted funding (from premium): 0.08%
//   - Actual funding rate (announced): 0.04%
//   - Action: LONG perp (rate lower than expected, premium will compress)
//
// FORMULAS:
// ---------
// 1. Funding rate differential (cross-exchange):
//    funding_diff = funding_A - funding_B
//
// 2. Annualized funding differential:
//    annual_diff = funding_diff * 3 * 365  (3 payments/day)
//
// 3. Z-score (mean reversion detection):
//    z_score = (current_funding - mean_funding) / std_funding
//
// 4. Predicted funding from premium:
//    predicted_funding = (perp_price - spot_price) / spot_price
//
// 5. Funding prediction error:
//    error = predicted_funding - actual_funding
//
// 6. Net profit (cross-exchange, per funding period):
//    profit = |funding_diff| * position_size - fees
//
// EXAMPLE 1: Cross-Exchange Funding Arb
// --------------------------------------
// Exchange A (Binance): Funding = +0.10% (longs pay shorts)
// Exchange B (Bybit): Funding = -0.05% (shorts pay longs)
//
// Setup:
//   - Capital: $100,000
//   - Position: LONG $100k on Bybit, SHORT $100k on Binance
//
// Every 8 hours:
//   - Bybit: Receive 0.05% = $50
//   - Binance: Receive 0.10% = $100 (we're short, so we collect)
//   - Total: $150 per 8 hours = $450/day = $164,250/year (164% APY!)
//
// Costs:
//   - Entry/exit fees: ~0.10% round trip = $200 (one-time)
//   - Delta hedging slippage: ~$100/day
//   - Net: ~$350/day = 128% APY
//
// Risks:
//   - Funding rate convergence (rates become equal)
//   - Basis risk (perp prices diverge between exchanges)
//   - Margin calls if one position moves against us
//
// EXAMPLE 2: Funding Rate Mean Reversion
// ---------------------------------------
// BTC Funding History (30-day):
//   - Mean: 0.01%
//   - Std: 0.04%
//
// Current:
//   - Funding: 0.21%
//   - Z-score: (0.21 - 0.01) / 0.04 = 5.0 (extremely high)
//
// Setup:
//   - SHORT perp (bet funding will decrease)
//   - Delta hedge with spot long (keep delta neutral)
//
// Expected:
//   - Funding reverts to 0.05% (1 std above mean)
//   - Basis compresses by 0.16%
//   - Profit: 0.16% on position
//
// WHEN TO USE:
// ------------
// 1. High funding differential: |diff| > 0.15% across exchanges
// 2. Extreme funding z-score: |z| > 3.0 (mean reversion opportunity)
// 3. Volatile markets: Funding rates fluctuate more
// 4. Before funding time: Rates often normalize 1-2 hours before payment
//
// WHY IT WORKS:
// -------------
// - Exchanges have different user bases (Binance retail, Bybit Asian)
// - Sentiment differences cause funding divergence
// - Funding rates are sticky (don't adjust instantly)
// - Arbitrageurs are capital-constrained (can't eliminate all divergence)
// - Mean reversion is a fundamental statistical property
//
// RISKS:
// ------
// 1. Convergence time: May take multiple funding periods
// 2. Funding expansion: Difference can widen before narrowing
// 3. Exchange risk: Bankruptcy, hack, withdrawal freeze
// 4. Margin risk: Need collateral for perp positions
// 5. Delta risk: Imperfect hedging leads to directional exposure
// 6. Execution risk: Can't enter both legs simultaneously
//
// IMPLEMENTATION NOTES:
// ---------------------
// - Monitor funding rates across all major exchanges
// - Calculate historical statistics (30-day rolling window)
// - Use z-score > 2.5 for mean reversion signals
// - Use funding_diff > 0.10% for cross-exchange signals
// - Delta hedge continuously (perp notional = hedge notional)
// - Exit at z-score < 1.0 or funding_diff < 0.03%
// - Position size: Max 50% of account per exchange
//
// TYPICAL PARAMETERS:
// -------------------
// - Min funding differential: 0.10% (1000 bps annualized)
// - Z-score threshold: ±2.5 (mean reversion)
// - Exit z-score: ±1.0 (return to normal)
// - Max holding periods: 24 hours (3 funding cycles)
// - Historical window: 30 days (90 funding periods)
//
//==============================================================================

#pragma once

#include "arbitrage_opportunity.hpp"
#include <map>
#include <vector>
#include <deque>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <chrono>
#include <string>

namespace arb {

/**
 * FundingRateData: Funding rate information for one exchange/product
 */
struct FundingRateData {
    std::string exchange;
    std::string product;
    double funding_rate;              // Current 8-hour funding rate (decimal)
    double hours_to_funding;          // Hours until next payment
    double predicted_funding;         // Predicted from current premium
    std::chrono::system_clock::time_point timestamp;
};

/**
 * FundingRateStats: Historical statistics for mean reversion analysis
 */
struct FundingRateStats {
    double mean;           // Historical mean funding rate
    double std_dev;        // Standard deviation
    double z_score;        // Current z-score
    size_t sample_size;    // Number of historical samples
};

/**
 * FundingRateArbitrage: Exploit funding rate inefficiencies
 */
class FundingRateArbitrage {
public:
    /**
     * Constructor
     * 
     * @param min_funding_diff: Minimum funding differential for cross-exchange arb (default 0.0010 = 0.10%)
     * @param z_score_threshold: Z-score threshold for mean reversion (default 2.5)
     * @param historical_window: Number of funding periods to track (default 90 = 30 days)
     */
    FundingRateArbitrage(double min_funding_diff = 0.0010,
                         double z_score_threshold = 2.5,
                         size_t historical_window = 90)
        : min_funding_diff_(min_funding_diff),
          z_score_threshold_(z_score_threshold),
          historical_window_(historical_window) {}

    /**
     * update_funding_rate: Store funding rate for historical tracking
     * 
     * @param data: Funding rate data from exchange
     */
    void update_funding_rate(const FundingRateData& data) {
        std::string key = data.exchange + ":" + data.product;
        
        // Add to historical data
        auto& history = funding_history_[key];
        history.push_back(data.funding_rate);
        
        // Keep only recent history
        if (history.size() > historical_window_) {
            history.pop_front();
        }
        
        // Update current rates
        current_rates_[key] = data;
    }
    
    /**
     * calculate_stats: Calculate mean, std dev, and z-score for a product
     * 
     * @param exchange: Exchange name
     * @param product: Product identifier
     * @return Statistics if sufficient history exists
     */
    std::optional<FundingRateStats> calculate_stats(const std::string& exchange,
                                                     const std::string& product) {
        std::string key = exchange + ":" + product;
        
        auto it = funding_history_.find(key);
        if (it == funding_history_.end() || it->second.size() < 10) {
            return std::nullopt;  // Need at least 10 samples
        }
        
        const auto& history = it->second;
        
        // Calculate mean
        double sum = std::accumulate(history.begin(), history.end(), 0.0);
        double mean = sum / history.size();
        
        // Calculate standard deviation
        double sq_sum = 0.0;
        for (double rate : history) {
            sq_sum += (rate - mean) * (rate - mean);
        }
        double std_dev = std::sqrt(sq_sum / history.size());
        
        // Get current rate
        auto curr_it = current_rates_.find(key);
        if (curr_it == current_rates_.end()) {
            return std::nullopt;
        }
        double current_rate = curr_it->second.funding_rate;
        
        // Calculate z-score
        double z_score = (std_dev > 0) ? (current_rate - mean) / std_dev : 0.0;
        
        FundingRateStats stats;
        stats.mean = mean;
        stats.std_dev = std_dev;
        stats.z_score = z_score;
        stats.sample_size = history.size();
        
        return stats;
    }
    
    /**
     * find_cross_exchange_opportunity: Find funding rate differentials across exchanges
     * 
     * ALGORITHM:
     * 1. Compare funding rates for same product across exchanges
     * 2. Calculate differential (absolute value)
     * 3. Check if differential exceeds threshold
     * 4. Determine which exchange to long vs short
     * 5. Calculate expected profit
     * 
     * @param product: Product to check (e.g., "BTC-USD")
     * @return ArbOpportunity if found
     */
    std::optional<ArbOpportunity> find_cross_exchange_opportunity(const std::string& product) {
        // Collect all exchanges with data for this product
        std::vector<FundingRateData> rates;
        for (const auto& [key, data] : current_rates_) {
            if (data.product == product) {
                rates.push_back(data);
            }
        }
        
        // Need at least 2 exchanges to arbitrage
        if (rates.size() < 2) {
            return std::nullopt;
        }
        
        // Find maximum differential
        double max_diff = 0.0;
        size_t high_idx = 0;
        size_t low_idx = 0;
        
        for (size_t i = 0; i < rates.size(); ++i) {
            for (size_t j = i + 1; j < rates.size(); ++j) {
                double diff = std::abs(rates[i].funding_rate - rates[j].funding_rate);
                if (diff > max_diff) {
                    max_diff = diff;
                    if (rates[i].funding_rate > rates[j].funding_rate) {
                        high_idx = i;
                        low_idx = j;
                    } else {
                        high_idx = j;
                        low_idx = i;
                    }
                }
            }
        }
        
        // Check if differential is large enough
        if (max_diff < min_funding_diff_) {
            return std::nullopt;
        }
        
        // Create opportunity
        ArbOpportunity opp;
        opp.type = ArbType::CROSS_EXCHANGE;
        opp.product = product;
        opp.timestamp = std::chrono::system_clock::now();
        
        // Strategy: LONG on low-funding exchange, SHORT on high-funding exchange
        // Why: If high funding is positive, shorts collect; if negative, longs pay less
        opp.buy_exchange = rates[low_idx].exchange;   // Long here (lower funding)
        opp.sell_exchange = rates[high_idx].exchange; // Short here (higher funding)
        
        // For funding arb, prices don't matter as much (it's market neutral)
        // Set to 0 as placeholder
        opp.buy_price = 0.0;
        opp.sell_price = 0.0;
        opp.quantity = 1.0;  // Normalized to 1 unit
        
        // Calculate profit
        // Differential in basis points
        opp.gross_spread_bps = max_diff * 10000.0;
        
        // Fees are minimal since we're not trading frequently
        // Mainly entry/exit costs amortized over holding period
        const double amortized_fee_bps = 2.0;  // ~0.02% per funding cycle
        opp.net_spread_bps = opp.gross_spread_bps - amortized_fee_bps;
        
        // Expected profit per funding period (8 hours)
        // Assuming $100k position
        const double position_size = 100000.0;
        opp.expected_profit_usd = (opp.net_spread_bps / 10000.0) * position_size;
        
        // Confidence based on:
        // 1. Size of differential (larger = more confident)
        // 2. Time to funding (closer = more certain)
        // 3. Historical stability (lower std dev = more predictable)
        const double diff_score = std::min(max_diff / 0.003, 1.0) * 0.60;  // Max 0.60
        const double time_score = std::min(
            (8.0 - rates[high_idx].hours_to_funding) / 8.0, 1.0) * 0.20;  // Max 0.20
        
        // Get historical stats for confidence
        auto stats_high = calculate_stats(rates[high_idx].exchange, product);
        auto stats_low = calculate_stats(rates[low_idx].exchange, product);
        double stability_score = 0.20;  // Default
        if (stats_high && stats_low) {
            double avg_std = (stats_high->std_dev + stats_low->std_dev) / 2.0;
            stability_score = std::max(0.0, 0.20 * (1.0 - avg_std / 0.01));  // Lower std = better
        }
        
        opp.confidence = std::min(diff_score + time_score + stability_score, 1.0);
        
        return opp;
    }
    
    /**
     * find_mean_reversion_opportunity: Find extreme funding rates likely to revert
     * 
     * ALGORITHM:
     * 1. Calculate z-score for current funding rate
     * 2. Check if |z-score| > threshold
     * 3. Determine trade direction (short if high funding, long if low)
     * 4. Estimate profit from reversion to mean
     * 
     * @param exchange: Exchange to check
     * @param product: Product to check
     * @return ArbOpportunity if found
     */
    std::optional<ArbOpportunity> find_mean_reversion_opportunity(
        const std::string& exchange,
        const std::string& product) {
        
        auto stats = calculate_stats(exchange, product);
        if (!stats) {
            return std::nullopt;  // Insufficient history
        }
        
        // Check if z-score is extreme
        if (std::abs(stats->z_score) < z_score_threshold_) {
            return std::nullopt;
        }
        
        // Get current rate
        std::string key = exchange + ":" + product;
        auto it = current_rates_.find(key);
        if (it == current_rates_.end()) {
            return std::nullopt;
        }
        const auto& data = it->second;
        
        // Create opportunity
        ArbOpportunity opp;
        opp.type = ArbType::STATISTICAL;
        opp.product = product;
        opp.timestamp = std::chrono::system_clock::now();
        
        // Trade direction based on z-score
        if (stats->z_score > 0) {
            // Funding is abnormally high → SHORT (bet it decreases)
            opp.buy_exchange = "spot_hedge";  // Will hedge with spot
            opp.sell_exchange = exchange;     // Short the perp
        } else {
            // Funding is abnormally low → LONG (bet it increases)
            opp.buy_exchange = exchange;      // Long the perp
            opp.sell_exchange = "spot_hedge"; // Will short spot to hedge
        }
        
        opp.buy_price = 0.0;   // Placeholder
        opp.sell_price = 0.0;
        opp.quantity = 1.0;
        
        // Expected reversion: from current to 1 std deviation from mean
        const double current_funding = data.funding_rate;
        const double expected_funding = stats->mean + 
            (stats->z_score > 0 ? stats->std_dev : -stats->std_dev);
        const double reversion_magnitude = std::abs(current_funding - expected_funding);
        
        opp.gross_spread_bps = reversion_magnitude * 10000.0;
        
        // More conservative fees for mean reversion (may take multiple periods)
        const double fee_bps = 15.0;  // Entry + exit + slippage
        opp.net_spread_bps = opp.gross_spread_bps - fee_bps;
        
        // Expected profit (assuming reversion over 1-2 funding periods)
        const double position_size = 100000.0;
        opp.expected_profit_usd = (opp.net_spread_bps / 10000.0) * position_size;
        
        // Confidence based on z-score magnitude and sample size
        const double z_score_conf = std::min(std::abs(stats->z_score) / 5.0, 1.0) * 0.60;
        const double sample_conf = std::min(static_cast<double>(stats->sample_size) / 90.0, 1.0) * 0.40;
        
        opp.confidence = std::min(z_score_conf + sample_conf, 1.0);
        
        return (opp.net_spread_bps > 0) ? std::optional<ArbOpportunity>(opp) : std::nullopt;
    }
    
    /**
     * find_prediction_error_opportunity: Exploit differences between predicted and actual funding
     * 
     * Predicted funding is calculated from the current premium:
     *   predicted = (perp_price - spot_price) / spot_price
     * 
     * If predicted ≠ actual, there's an opportunity
     * 
     * @param data: Funding rate data with predicted funding
     * @return ArbOpportunity if found
     */
    std::optional<ArbOpportunity> find_prediction_error_opportunity(
        const FundingRateData& data) {
        
        // Calculate prediction error
        const double error = data.predicted_funding - data.funding_rate;
        const double error_bps = std::abs(error) * 10000.0;
        
        // Need at least 10 bps error to be worthwhile
        if (error_bps < 10.0) {
            return std::nullopt;
        }
        
        ArbOpportunity opp;
        opp.type = ArbType::STATISTICAL;
        opp.product = data.product;
        opp.timestamp = data.timestamp;
        
        if (error > 0) {
            // Predicted > actual: Premium is too high, will compress
            // Action: SHORT perp (sell premium)
            opp.buy_exchange = "spot_hedge";
            opp.sell_exchange = data.exchange;
        } else {
            // Predicted < actual: Premium is too low, will expand
            // Action: LONG perp (buy premium)
            opp.buy_exchange = data.exchange;
            opp.sell_exchange = "spot_hedge";
        }
        
        opp.buy_price = 0.0;
        opp.sell_price = 0.0;
        opp.quantity = 1.0;
        
        opp.gross_spread_bps = error_bps;
        const double fee_bps = 12.0;
        opp.net_spread_bps = opp.gross_spread_bps - fee_bps;
        
        const double position_size = 100000.0;
        opp.expected_profit_usd = (opp.net_spread_bps / 10000.0) * position_size;
        
        // Confidence based on error magnitude and time to funding
        const double error_conf = std::min(error_bps / 50.0, 1.0) * 0.70;
        const double time_conf = (1.0 - data.hours_to_funding / 8.0) * 0.30;
        
        opp.confidence = std::min(error_conf + time_conf, 1.0);
        
        return (opp.net_spread_bps > 0) ? std::optional<ArbOpportunity>(opp) : std::nullopt;
    }

private:
    double min_funding_diff_;          // Min funding differential for cross-exchange
    double z_score_threshold_;         // Z-score threshold for mean reversion
    size_t historical_window_;         // Number of periods to track
    
    // Historical funding rates: "exchange:product" -> deque of rates
    std::map<std::string, std::deque<double>> funding_history_;
    
    // Current funding rates: "exchange:product" -> latest data
    std::map<std::string, FundingRateData> current_rates_;
};

} // namespace arb
