//==============================================================================
// PERPETUAL-SPOT ARBITRAGE STRATEGY
//==============================================================================
//
// THEORY & RATIONALE:
// -------------------
// Perpetual futures (perps) are derivative contracts with no expiration date.
// They track spot prices through a "funding rate" mechanism:
//   - When perp price > spot price → longs pay shorts (positive funding)
//   - When perp price < spot price → shorts pay longs (negative funding)
//   - Funding payments occur every 8 hours (typical)
//
// ARBITRAGE OPPORTUNITY:
// ----------------------
// When the basis (perp - spot) deviates from expected funding, we can arbitrage:
//
// Case 1: Perp overpriced (positive basis > funding rate)
//   Action: SHORT perp + LONG spot
//   Profit: Basis convergence + collect funding from longs
//
// Case 2: Perp underpriced (negative basis < -funding rate)
//   Action: LONG perp + SHORT spot (or sell spot holdings)
//   Profit: Basis convergence + pay minimal funding
//
// FORMULAS:
// ---------
// 1. Basis (bps):
//    basis_bps = ((perp_mid - spot_mid) / spot_mid) * 10000
//
// 2. Annualized funding rate (assuming 3 payments/day):
//    annual_funding = funding_rate * 3 * 365
//
// 3. Expected basis (bps) for time T until next funding:
//    expected_basis = (funding_rate * T_hours / 8) * 10000
//
// 4. Basis deviation (the arbitrage signal):
//    deviation_bps = basis_bps - expected_basis_bps
//
// 5. Entry condition:
//    |deviation_bps| > threshold (e.g., 25 bps)
//
// 6. Profit potential (per 8-hour funding period):
//    gross_profit = |deviation_bps| + |funding_rate * 10000|
//    net_profit = gross_profit - fees - slippage
//
// EXAMPLE:
// --------
// Spot BTC: $50,000
// Perp BTC: $50,100
// Funding rate: +0.01% (longs pay shorts)
// Time to next funding: 4 hours
//
// Calculation:
//   basis = (50100 - 50000) / 50000 * 10000 = 20 bps
//   expected_basis = (0.01 * 4 / 8) * 10000 = 5 bps
//   deviation = 20 - 5 = 15 bps
//
// If threshold = 10 bps → TRADE
//   Action: Short perp @ $50,100, Long spot @ $50,000
//   In 4 hours at funding:
//     - Collect 0.01% funding = $5.01
//     - If basis normalizes to 5 bps: $50,025 - $50,000 = $25 profit
//     - Total: $30.01 per BTC (0.06% return in 4 hours = 131% APY!)
//   Costs:
//     - Perp taker fee: 0.05% = $25.05
//     - Spot taker fee: 0.10% = $50.00
//     - Total fees: $75.05
//   Net: $30.01 - $75.05 = -$45.04 loss
//
// This shows why we need deviation > ~20-30 bps to be profitable!
//
// WHEN TO USE:
// ------------
// - High volatility periods: Basis deviations are larger
// - Funding rate flips: When funding changes sign, basis overshoots
// - Exchange maintenance: When one venue has reduced liquidity
// - Capital: Works best with size ($100k+) to amortize fixed costs
//
// WHY IT WORKS:
// -------------
// - Funding mechanism is imperfect and lags price movements
// - Traders overreact to short-term volatility in perps
// - Spot has different liquidity profile than perps
// - Cross-exchange inefficiencies (Binance perp vs Coinbase spot)
//
// RISKS:
// ------
// 1. Execution risk: Prices move while placing both legs
// 2. Funding rate risk: Rate changes before collection
// 3. Margin risk: Perp position requires collateral
// 4. Exchange risk: One exchange goes down mid-trade
// 5. Basis expansion: Deviation worsens before improving
//
// IMPLEMENTATION NOTES:
// ---------------------
// - Use limit orders on both legs to minimize fees
// - Monitor margin utilization (avoid liquidation)
// - Set stop-loss at 2x expected basis range
// - Exit if funding rate changes dramatically
// - Hedge delta continuously (perp notional = spot notional)
//
// TYPICAL PARAMETERS:
// -------------------
// - Entry threshold: 20-40 bps deviation
// - Exit threshold: 5-10 bps deviation
// - Max holding time: Until next funding + 1 hour
// - Min profit target: 15 bps net after fees
// - Max position size: 30% of account (due to margin requirements)
//
//==============================================================================

#pragma once

#include "arbitrage_opportunity.hpp"
#include <cmath>
#include <chrono>
#include <string>

namespace arb {

/**
 * PerpSpotQuote: Combined quote data for perp and spot
 */
struct PerpSpotQuote {
    std::string product;               // e.g., "BTC-USD"
    
    double spot_bid;                   // Spot best bid
    double spot_ask;                   // Spot best ask
    double spot_bid_size;              // Spot bid quantity
    double spot_ask_size;              // Spot ask quantity
    std::string spot_exchange;         // e.g., "coinbase"
    
    double perp_bid;                   // Perp best bid
    double perp_ask;                   // Perp best ask
    double perp_bid_size;              // Perp bid quantity
    double perp_ask_size;              // Perp ask quantity
    std::string perp_exchange;         // e.g., "binance"
    
    double funding_rate;               // Current 8-hour funding rate (decimal)
    double hours_to_funding;           // Hours until next funding payment
    
    std::chrono::system_clock::time_point timestamp;
};

/**
 * PerpSpotArbitrage: Arbitrage between perpetual futures and spot
 */
class PerpSpotArbitrage {
public:
    /**
     * Constructor
     * 
     * @param deviation_threshold_bps: Minimum basis deviation to trigger trade (default 25)
     * @param exit_threshold_bps: Basis deviation to close position (default 8)
     * @param max_funding_rate: Maximum absolute funding rate to trade (default 0.001 = 0.1%)
     */
    PerpSpotArbitrage(double deviation_threshold_bps = 25.0,
                      double exit_threshold_bps = 8.0,
                      double max_funding_rate = 0.001)
        : deviation_threshold_bps_(deviation_threshold_bps),
          exit_threshold_bps_(exit_threshold_bps),
          max_funding_rate_(max_funding_rate) {}

    /**
     * find_opportunity: Analyze perp-spot pair for arbitrage
     * 
     * ALGORITHM:
     * 1. Calculate mid prices for both perp and spot
     * 2. Calculate basis in basis points
     * 3. Calculate expected basis from funding rate
     * 4. Calculate deviation from expected basis
     * 5. Check if deviation exceeds threshold
     * 6. Determine trade direction and expected profit
     * 
     * @param quote: Combined perp and spot quote data
     * @return ArbOpportunity if found, empty optional otherwise
     */
    std::optional<ArbOpportunity> find_opportunity(const PerpSpotQuote& quote) {
        // Calculate mid prices
        const double spot_mid = (quote.spot_bid + quote.spot_ask) / 2.0;
        const double perp_mid = (quote.perp_bid + quote.perp_ask) / 2.0;
        
        // Validate prices
        if (spot_mid <= 0 || perp_mid <= 0) return std::nullopt;
        if (quote.spot_bid_size <= 0 || quote.spot_ask_size <= 0) return std::nullopt;
        if (quote.perp_bid_size <= 0 || quote.perp_ask_size <= 0) return std::nullopt;
        
        // Calculate basis in basis points
        const double basis_bps = ((perp_mid - spot_mid) / spot_mid) * 10000.0;
        
        // Calculate expected basis from funding rate and time to funding
        // Expected basis = funding_rate * (hours_to_funding / 8) * 10000
        const double expected_basis_bps = 
            (quote.funding_rate * quote.hours_to_funding / 8.0) * 10000.0;
        
        // Calculate deviation (the arbitrage signal)
        const double deviation_bps = basis_bps - expected_basis_bps;
        
        // Check if absolute funding rate is too high (too risky)
        if (std::abs(quote.funding_rate) > max_funding_rate_) {
            return std::nullopt;
        }
        
        // Check if deviation is large enough to trade
        if (std::abs(deviation_bps) < deviation_threshold_bps_) {
            return std::nullopt;
        }
        
        // Determine trade direction and calculate expected profit
        ArbOpportunity opp;
        opp.type = ArbType::CROSS_EXCHANGE;  // Perp-spot is cross-exchange arb
        opp.product = quote.product;
        opp.timestamp = quote.timestamp;
        
        if (deviation_bps > 0) {
            // Perp is overpriced relative to expected funding
            // Action: SHORT perp, LONG spot
            opp.buy_exchange = quote.spot_exchange;
            opp.sell_exchange = quote.perp_exchange;
            opp.buy_price = quote.spot_ask;   // Pay ask to buy spot
            opp.sell_price = quote.perp_bid;  // Receive bid to short perp
            opp.quantity = std::min(quote.spot_ask_size, quote.perp_bid_size);
            
        } else {
            // Perp is underpriced relative to expected funding
            // Action: LONG perp, SHORT spot
            opp.buy_exchange = quote.perp_exchange;
            opp.sell_exchange = quote.spot_exchange;
            opp.buy_price = quote.perp_ask;   // Pay ask to buy perp
            opp.sell_price = quote.spot_bid;  // Receive bid to sell spot
            opp.quantity = std::min(quote.perp_ask_size, quote.spot_bid_size);
        }
        
        // Calculate gross spread in basis points
        opp.gross_spread_bps = std::abs(deviation_bps);
        
        // Estimate fees (typical: perp 0.05% taker, spot 0.10% taker)
        const double perp_fee_bps = 5.0;   // 0.05% = 5 bps
        const double spot_fee_bps = 10.0;  // 0.10% = 10 bps
        const double total_fee_bps = perp_fee_bps + spot_fee_bps;
        
        // Net spread after fees
        opp.net_spread_bps = opp.gross_spread_bps - total_fee_bps;
        
        // Expected profit includes funding collection
        // If short perp with positive funding, we collect funding
        const double funding_profit_bps = std::abs(quote.funding_rate) * 10000.0;
        opp.expected_profit_usd = 
            (opp.net_spread_bps + funding_profit_bps) / 10000.0 * spot_mid * opp.quantity;
        
        // Confidence based on:
        // 1. Size available (larger = higher confidence)
        // 2. Funding rate stability (closer to 0 = more stable)
        // 3. Time to funding (closer = higher confidence)
        const double size_score = std::min(opp.quantity / 10.0, 1.0) * 0.40;  // Max 0.40
        const double funding_score = (1.0 - std::abs(quote.funding_rate) / max_funding_rate_) * 0.30;
        const double time_score = (1.0 - quote.hours_to_funding / 8.0) * 0.30;  // Closer to funding = better
        
        opp.confidence = std::min(size_score + funding_score + time_score, 1.0);
        
        // Only return if net profit is positive
        return (opp.net_spread_bps > 0) ? std::optional<ArbOpportunity>(opp) : std::nullopt;
    }
    
    /**
     * should_exit: Check if existing position should be closed
     * 
     * Exit conditions:
     * 1. Deviation has narrowed to exit threshold
     * 2. Funding rate changed significantly
     * 3. Max holding time reached
     * 
     * @param quote: Current market data
     * @param entry_deviation_bps: Deviation when position was entered
     * @param entry_funding_rate: Funding rate when position was entered
     * @param hours_held: How long position has been held
     * @return true if should exit position
     */
    bool should_exit(const PerpSpotQuote& quote, 
                     double entry_deviation_bps,
                     double entry_funding_rate,
                     double hours_held) {
        // Calculate current deviation
        const double spot_mid = (quote.spot_bid + quote.spot_ask) / 2.0;
        const double perp_mid = (quote.perp_bid + quote.perp_ask) / 2.0;
        const double basis_bps = ((perp_mid - spot_mid) / spot_mid) * 10000.0;
        const double expected_basis_bps = 
            (quote.funding_rate * quote.hours_to_funding / 8.0) * 10000.0;
        const double current_deviation_bps = basis_bps - expected_basis_bps;
        
        // Exit if deviation narrowed sufficiently
        if (std::abs(current_deviation_bps) < exit_threshold_bps_) {
            return true;
        }
        
        // Exit if funding rate changed dramatically (>50% change)
        if (std::abs(quote.funding_rate - entry_funding_rate) > 
            std::abs(entry_funding_rate) * 0.5) {
            return true;
        }
        
        // Exit if held too long (past funding + 1 hour grace period)
        if (hours_held > 9.0) {
            return true;
        }
        
        // Exit if deviation expanded too much (stop loss at 2x entry)
        if (std::abs(current_deviation_bps) > std::abs(entry_deviation_bps) * 2.0) {
            return true;
        }
        
        return false;
    }

private:
    double deviation_threshold_bps_;  // Min deviation to enter trade
    double exit_threshold_bps_;       // Max deviation to exit trade
    double max_funding_rate_;         // Max acceptable funding rate
};

} // namespace arb
