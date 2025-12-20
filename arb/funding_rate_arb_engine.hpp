#pragma once

//==============================================================================
// FUNDING RATE ARBITRAGE ENGINE
//==============================================================================
// Comprehensive engine for funding rate arbitrage opportunities
// 
// STRATEGY TYPES:
// 1. PERP-PERP ARBITRAGE: Cross-exchange funding rate differentials
// 2. PERP-SPOT ARBITRAGE: Perpetual vs spot with funding capture
//
// FUNDING RATE LOGIC:
// -------------------
// • Positive Funding Rate: Longs pay shorts → SHORT perp, LONG spot
// • Negative Funding Rate: Shorts pay longs → LONG perp, SHORT spot
//
// PROFIT CALCULATION:
// -------------------
// Perp-Perp:
//   Net APY = |Funding Rate High| - |Funding Rate Low| - (2 × Trading Fees)
//   Position: Long exchange with LOWER rate, Short exchange with HIGHER rate
//   
// Perp-Spot:
//   If Funding Rate > 0 (longs pay shorts):
//     → SHORT perp, LONG spot
//     → Collect funding from perp shorts
//     → Net APY = Funding Rate APY - Perp Fee - Spot Fee
//   If Funding Rate < 0 (shorts pay longs):
//     → LONG perp, SHORT spot
//     → Collect funding from perp longs
//     → Net APY = |Funding Rate APY| - Perp Fee - Spot Fee
//
// INTEGRATION:
// ------------
// Uses normalized data from multi_exchange_pipeline.hpp with SPSC queues
//==============================================================================

#include "normalized_exchange_data.hpp"
#include "multi_exchange_pipeline.hpp"
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>

namespace arb {

//==============================================================================
// ARBITRAGE OPPORTUNITY TYPES
//==============================================================================

enum class ArbType {
    PERP_PERP,   // Cross-exchange perpetual arbitrage
    PERP_SPOT    // Perpetual-spot arbitrage
};

//==============================================================================
// FUNDING RATE ARBITRAGE OPPORTUNITY
//==============================================================================

struct FundingArbOpportunity {
    ArbType type;
    UnifiedSymbol symbol;
    
    // For PERP-PERP
    ExchangeID long_exchange;     // Where to go LONG (lower|negative funding)
    ExchangeID short_exchange;    // Where to go SHORT (higher|positive funding)
    double long_funding_apy;      // APY on long side
    double short_funding_apy;     // APY on short side
    
    // For PERP-SPOT
    ExchangeID perp_exchange;     // Perpetual exchange
    double perp_funding_apy;      // Perpetual funding APY
    bool short_perp;              // true = short perp + long spot
    
    // Common fields
    double gross_spread_apy;      // Raw spread before fees
    double trading_fees_apy;      // Annualized trading fees
    double net_profit_apy;        // Net profit after all costs
    
    // Execution details
    double long_price;            // Entry price for long position
    double short_price;           // Entry price for short position
    double price_diff_bps;        // Price difference in basis points
    
    // Risk metrics
    double min_liquidity;         // Minimum liquidity on either side
    int num_exchanges;            // How many exchanges have this symbol
    double confidence;            // Confidence score (0-1)
    
    int64_t timestamp_ns;         // When opportunity detected
    
    // Helper methods
    std::string to_string() const {
        std::ostringstream oss;
        
        if (type == ArbType::PERP_PERP) {
            oss << "[PERP-PERP] " << symbol.to_string()
                << ": Long " << exchange_to_string(long_exchange)
                << " @" << std::fixed << std::setprecision(2) << long_funding_apy << "%"
                << ", Short " << exchange_to_string(short_exchange)
                << " @" << short_funding_apy << "%"
                << " | Net: " << net_profit_apy << "% APY"
                << " | Confidence: " << std::setprecision(0) << (confidence * 100) << "%";
        } else {
            oss << "[PERP-SPOT] " << symbol.to_string()
                << " @" << exchange_to_string(perp_exchange)
                << ": " << (short_perp ? "Short Perp + Long Spot" : "Long Perp + Short Spot")
                << " | Funding: " << std::fixed << std::setprecision(2) << perp_funding_apy << "%"
                << " | Net: " << net_profit_apy << "% APY"
                << " | Confidence: " << std::setprecision(0) << (confidence * 100) << "%";
        }
        
        return oss.str();
    }
};

//==============================================================================
// FUNDING RATE ARBITRAGE ENGINE
//==============================================================================

class FundingRateArbEngine {
public:
    struct Config {
        double min_perp_perp_spread_apy = 15.0;   // Minimum spread for perp-perp arb
        double min_perp_spot_rate_apy = 20.0;     // Minimum funding rate for perp-spot
        double max_price_diff_bps = 50.0;         // Max price divergence (risk control)
        double min_liquidity = 10000.0;           // Minimum $10k liquidity
        double min_confidence = 0.70;             // Minimum 70% confidence
        bool enable_perp_perp = true;
        bool enable_perp_spot = true;
    };
    
    FundingRateArbEngine(const Config& config)
        : config_(config) {}
    
    FundingRateArbEngine()
        : config_(Config{}) {}
    
    //==========================================================================
    // FIND OPPORTUNITIES FROM UNIFIED MARKET DATA
    //==========================================================================
    
    std::vector<FundingArbOpportunity> find_opportunities(
        const std::vector<UnifiedMarketData>& market_data) const {
        
        // Group data by symbol
        std::map<UnifiedSymbol, std::vector<const UnifiedMarketData*>> by_symbol;
        
        for (const auto& data : market_data) {
            // Check if orderbook has valid prices
            bool has_valid_orderbook = (data.orderbook.best_bid_price > 0 && 
                                       data.orderbook.best_ask_price > 0 &&
                                       !data.orderbook.bids.empty() &&
                                       !data.orderbook.asks.empty());
            
            if (data.has_funding() && has_valid_orderbook) {
                by_symbol[data.orderbook.unified_symbol].push_back(&data);
            }
        }
        
        std::vector<FundingArbOpportunity> opportunities;
        
        // Find opportunities for each symbol
        for (const auto& [symbol, data_list] : by_symbol) {
            if (data_list.size() < 2) continue;
            
            // Perp-Perp arbitrage
            if (config_.enable_perp_perp) {
                auto perp_perp_opps = find_perp_perp_opportunities(symbol, data_list);
                opportunities.insert(opportunities.end(), 
                                   perp_perp_opps.begin(), perp_perp_opps.end());
            }
            
            // Perp-Spot arbitrage
            if (config_.enable_perp_spot) {
                auto perp_spot_opps = find_perp_spot_opportunities(symbol, data_list);
                opportunities.insert(opportunities.end(),
                                   perp_spot_opps.begin(), perp_spot_opps.end());
            }
        }
        
        // Sort by net profit
        std::sort(opportunities.begin(), opportunities.end(),
            [](const FundingArbOpportunity& a, const FundingArbOpportunity& b) {
                return a.net_profit_apy > b.net_profit_apy;
            });
        
        return opportunities;
    }
    
private:
    Config config_;
    
    //==========================================================================
    // PERP-PERP ARBITRAGE DETECTION
    //==========================================================================
    // Strategy: Go long on exchange with LOWER funding, short on HIGHER funding
    // Collect the spread as profit
    //
    // CRITICAL: Sign handling!
    // - If both positive: Long lower (pay less), Short higher (collect more)
    // - If both negative: Long more negative (collect more), Short less negative (pay less)
    // - If mixed: Long negative (collect), Short positive (collect)
    //==========================================================================
    
    std::vector<FundingArbOpportunity> find_perp_perp_opportunities(
        const UnifiedSymbol& symbol,
        const std::vector<const UnifiedMarketData*>& data_list) const {
        
        std::vector<FundingArbOpportunity> opportunities;
        
        // Compare all pairs
        for (size_t i = 0; i < data_list.size(); ++i) {
            for (size_t j = i + 1; j < data_list.size(); ++j) {
                const auto* data_a = data_list[i];
                const auto* data_b = data_list[j];
                
                if (!data_a->has_funding() || !data_b->has_funding()) continue;
                
                double rate_a = data_a->funding->funding_rate_annual;
                double rate_b = data_b->funding->funding_rate_annual;
                
                // Determine which side to long and short
                const UnifiedMarketData* long_data;
                const UnifiedMarketData* short_data;
                
                // Key logic: We want to COLLECT on both sides if possible
                // Long the side with LOWER (more negative) rate
                // Short the side with HIGHER (more positive) rate
                if (rate_a < rate_b) {
                    long_data = data_a;   // Long lower rate
                    short_data = data_b;  // Short higher rate
                } else {
                    long_data = data_b;
                    short_data = data_a;
                }
                
                double long_rate = long_data->funding->funding_rate_annual;
                double short_rate = short_data->funding->funding_rate_annual;
                
                // Calculate gross spread
                // If long_rate is negative (we collect), short_rate is positive (we collect)
                // → gross spread = |short_rate| + |long_rate|
                // If both same sign, spread = |difference|
                double gross_spread_apy;
                if ((long_rate < 0 && short_rate > 0) || (long_rate > 0 && short_rate < 0)) {
                    // Opposite signs: we collect on both sides!
                    gross_spread_apy = std::abs(short_rate) + std::abs(long_rate);
                } else {
                    // Same sign: we net the difference
                    gross_spread_apy = std::abs(short_rate - long_rate);
                }
                
                // Trading fees (2 legs × 2 exchanges = 4 trades total)
                double long_fee = get_exchange_fee(long_data->orderbook.exchange_id);
                double short_fee = get_exchange_fee(short_data->orderbook.exchange_id);
                
                // Annualize trading fees (assuming rebalancing monthly)
                double trading_fees_apy = (long_fee + short_fee) * 12 * 100;
                
                double net_profit_apy = gross_spread_apy - trading_fees_apy;
                
                // Check if profitable
                if (net_profit_apy < config_.min_perp_perp_spread_apy) continue;
                
                // Price divergence check
                double price_diff_bps = std::abs(
                    (long_data->orderbook.mid_price() - short_data->orderbook.mid_price()) /
                    long_data->orderbook.mid_price() * 10000.0
                );
                
                if (price_diff_bps > config_.max_price_diff_bps) continue;
                
                // Liquidity check
                double long_liq = long_data->orderbook.best_bid_qty * long_data->orderbook.best_bid_price;
                double short_liq = short_data->orderbook.best_ask_qty * short_data->orderbook.best_ask_price;
                double min_liq = std::min(long_liq, short_liq);
                
                if (min_liq < config_.min_liquidity) continue;
                
                // Calculate confidence
                double confidence = calculate_confidence(
                    gross_spread_apy, price_diff_bps, min_liq, data_list.size());
                
                if (confidence < config_.min_confidence) continue;
                
                // Create opportunity
                FundingArbOpportunity opp;
                opp.type = ArbType::PERP_PERP;
                opp.symbol = symbol;
                opp.long_exchange = long_data->orderbook.exchange_id;
                opp.short_exchange = short_data->orderbook.exchange_id;
                opp.long_funding_apy = long_rate;
                opp.short_funding_apy = short_rate;
                opp.gross_spread_apy = gross_spread_apy;
                opp.trading_fees_apy = trading_fees_apy;
                opp.net_profit_apy = net_profit_apy;
                opp.long_price = long_data->orderbook.best_ask_price;
                opp.short_price = short_data->orderbook.best_bid_price;
                opp.price_diff_bps = price_diff_bps;
                opp.min_liquidity = min_liq;
                opp.num_exchanges = data_list.size();
                opp.confidence = confidence;
                opp.timestamp_ns = long_data->orderbook.local_timestamp_ns;
                
                opportunities.push_back(opp);
            }
        }
        
        return opportunities;
    }
    
    //==========================================================================
    // PERP-SPOT ARBITRAGE DETECTION  
    //==========================================================================
    // Strategy: Hold spot, hedge with perpetual, collect funding payments
    //
    // If funding rate > 0 (longs pay shorts):
    //   → SHORT perp + LONG spot → Collect funding from perp shorts
    //
    // If funding rate < 0 (shorts pay longs):
    //   → LONG perp + SHORT spot → Collect funding from perp longs
    //==========================================================================
    
    std::vector<FundingArbOpportunity> find_perp_spot_opportunities(
        const UnifiedSymbol& symbol,
        const std::vector<const UnifiedMarketData*>& data_list) const {
        
        std::vector<FundingArbOpportunity> opportunities;
        
        // For each perpetual, evaluate if funding rate justifies perp-spot arb
        for (const auto* data : data_list) {
            if (!data->has_funding()) continue;
            
            double funding_apy = data->funding->funding_rate_annual;
            
            // Check if funding rate is significant enough
            if (std::abs(funding_apy) < config_.min_perp_spot_rate_apy) continue;
            
            // Determine position direction
            bool short_perp = (funding_apy > 0);  // If positive, short perp to collect
            
            // Trading fees
            double perp_fee = get_exchange_fee(data->orderbook.exchange_id);
            double spot_fee = 0.001;  // Assume 0.1% spot fee (conservative)
            
            // Annualize fees (monthly rebalancing)
            double trading_fees_apy = (perp_fee + spot_fee) * 12 * 100;
            
            // Net profit = |funding rate| - fees
            double net_profit_apy = std::abs(funding_apy) - trading_fees_apy;
            
            if (net_profit_apy < config_.min_perp_spot_rate_apy) continue;
            
            // Liquidity check
            double liquidity = short_perp ?
                data->orderbook.best_bid_qty * data->orderbook.best_bid_price :
                data->orderbook.best_ask_qty * data->orderbook.best_ask_price;
            
            if (liquidity < config_.min_liquidity) continue;
            
            // Confidence calculation
            double confidence = calculate_confidence(
                std::abs(funding_apy), 0.0, liquidity, 1);
            
            if (confidence < config_.min_confidence) continue;
            
            // Create opportunity
            FundingArbOpportunity opp;
            opp.type = ArbType::PERP_SPOT;
            opp.symbol = symbol;
            opp.perp_exchange = data->orderbook.exchange_id;
            opp.perp_funding_apy = funding_apy;
            opp.short_perp = short_perp;
            opp.gross_spread_apy = std::abs(funding_apy);
            opp.trading_fees_apy = trading_fees_apy;
            opp.net_profit_apy = net_profit_apy;
            opp.long_price = short_perp ? 0 : data->orderbook.best_ask_price;
            opp.short_price = short_perp ? data->orderbook.best_bid_price : 0;
            opp.price_diff_bps = 0.0;
            opp.min_liquidity = liquidity;
            opp.num_exchanges = data_list.size();
            opp.confidence = confidence;
            opp.timestamp_ns = data->orderbook.local_timestamp_ns;
            
            opportunities.push_back(opp);
        }
        
        return opportunities;
    }
    
    //==========================================================================
    // HELPER FUNCTIONS
    //==========================================================================
    
    double get_exchange_fee(ExchangeID exchange_id) const {
        // Taker fees in decimal form
        switch (exchange_id) {
            case ExchangeID::BINANCE: return 0.0004;
            case ExchangeID::BYBIT: return 0.00055;
            case ExchangeID::OKX: return 0.0005;
            case ExchangeID::GATEIO: return 0.0005;
            case ExchangeID::MEXC: return 0.0006;
            case ExchangeID::KUCOIN: return 0.0006;
            case ExchangeID::KRAKEN: return 0.0005;
            case ExchangeID::BITGET: return 0.0006;
            case ExchangeID::HTX: return 0.0005;
            case ExchangeID::BINGX: return 0.0005;
            default: return 0.0006;  // Conservative default
        }
    }
    
    double calculate_confidence(double spread_apy, double price_diff_bps,
                                double liquidity, int num_exchanges) const {
        double confidence = 0.5;  // Base confidence
        
        // Higher spread = higher confidence
        if (spread_apy > 50.0) confidence += 0.2;
        else if (spread_apy > 30.0) confidence += 0.1;
        
        // Lower price diff = higher confidence
        if (price_diff_bps < 10.0) confidence += 0.2;
        else if (price_diff_bps < 25.0) confidence += 0.1;
        
        // Higher liquidity = higher confidence
        if (liquidity > 100000.0) confidence += 0.1;
        else if (liquidity > 50000.0) confidence += 0.05;
        
        // More exchanges = higher confidence
        if (num_exchanges >= 5) confidence += 0.1;
        else if (num_exchanges >= 3) confidence += 0.05;
        
        return std::min(confidence, 1.0);
    }
};

} // namespace arb
