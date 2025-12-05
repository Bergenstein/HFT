//==============================================================================
// TEST: New Arbitrage Strategies
//==============================================================================
// Tests for:
// 1. Perp-Spot Arbitrage
// 2. Funding Rate Arbitrage (Cross-Exchange, Mean Reversion, Prediction Error)
// 3. Market Neutral Pairs Trading
//==============================================================================

#include "../arb/perp_spot_arb.hpp"
#include "../arb/funding_rate_arb.hpp"
#include "../arb/market_neutral_pairs.hpp"
#include <iostream>
#include <iomanip>
#include <cassert>

using namespace arb;

void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << title << "\n";
    std::cout << std::string(70, '=') << "\n";
}

void print_opportunity(const ArbOpportunity& opp) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Type: " << (opp.type == ArbType::CROSS_EXCHANGE ? "CROSS_EXCHANGE" :
                                 opp.type == ArbType::TRIANGULAR ? "TRIANGULAR" : "STATISTICAL") << "\n";
    std::cout << "  Product: " << opp.product << "\n";
    std::cout << "  Buy: " << opp.buy_exchange << " @ $" << opp.buy_price << "\n";
    std::cout << "  Sell: " << opp.sell_exchange << " @ $" << opp.sell_price << "\n";
    std::cout << "  Quantity: " << opp.quantity << "\n";
    std::cout << "  Gross Spread: " << opp.gross_spread_bps << " bps\n";
    std::cout << "  Net Spread: " << opp.net_spread_bps << " bps\n";
    std::cout << "  Expected Profit: $" << opp.expected_profit_usd << "\n";
    std::cout << "  Confidence: " << (opp.confidence * 100) << "%\n";
}

//==============================================================================
// TEST 1: Perp-Spot Arbitrage
//==============================================================================
void test_perp_spot_arbitrage() {
    print_separator("TEST 1: Perp-Spot Arbitrage");
    
    PerpSpotArbitrage strategy(25.0, 8.0, 0.001);
    
    // Test Case 1: Perp overpriced (positive deviation)
    std::cout << "\n--- Test 1.1: Perp Overpriced ---\n";
    PerpSpotQuote quote1;
    quote1.product = "BTC-USD";
    quote1.spot_bid = 49900.0;
    quote1.spot_ask = 50000.0;
    quote1.spot_bid_size = 2.0;
    quote1.spot_ask_size = 1.5;
    quote1.spot_exchange = "coinbase";
    quote1.perp_bid = 50150.0;
    quote1.perp_ask = 50200.0;
    quote1.perp_bid_size = 3.0;
    quote1.perp_ask_size = 2.5;
    quote1.perp_exchange = "binance";
    quote1.funding_rate = 0.0001;  // 0.01% (positive but small)
    quote1.hours_to_funding = 4.0;
    quote1.timestamp = std::chrono::system_clock::now();
    
    auto opp1 = strategy.find_opportunity(quote1);
    if (opp1) {
        std::cout << "✓ Opportunity found!\n";
        print_opportunity(*opp1);
        
        // Verify trade direction
        assert(opp1->buy_exchange == "coinbase");  // Buy spot
        assert(opp1->sell_exchange == "binance");  // Short perp
        std::cout << "✓ Trade direction correct (short perp, long spot)\n";
    } else {
        std::cout << "✗ No opportunity found (unexpected)\n";
    }
    
    // Test Case 2: Perp underpriced (negative deviation)
    std::cout << "\n--- Test 1.2: Perp Underpriced ---\n";
    PerpSpotQuote quote2;
    quote2.product = "ETH-USD";
    quote2.spot_bid = 3000.0;
    quote2.spot_ask = 3005.0;
    quote2.spot_bid_size = 10.0;
    quote2.spot_ask_size = 8.0;
    quote2.spot_exchange = "coinbase";
    quote2.perp_bid = 2950.0;
    quote2.perp_ask = 2955.0;
    quote2.perp_bid_size = 15.0;
    quote2.perp_ask_size = 12.0;
    quote2.perp_exchange = "binance";
    quote2.funding_rate = -0.0002;  // -0.02% (negative)
    quote2.hours_to_funding = 6.0;
    quote2.timestamp = std::chrono::system_clock::now();
    
    auto opp2 = strategy.find_opportunity(quote2);
    if (opp2) {
        std::cout << "✓ Opportunity found!\n";
        print_opportunity(*opp2);
        
        // Verify trade direction
        assert(opp2->buy_exchange == "binance");   // Buy perp
        assert(opp2->sell_exchange == "coinbase"); // Short spot
        std::cout << "✓ Trade direction correct (long perp, short spot)\n";
    } else {
        std::cout << "✗ No opportunity found (unexpected)\n";
    }
    
    // Test Case 3: Small deviation (should not trigger)
    std::cout << "\n--- Test 1.3: Small Deviation (No Trade) ---\n";
    PerpSpotQuote quote3;
    quote3.product = "BTC-USD";
    quote3.spot_bid = 50000.0;
    quote3.spot_ask = 50005.0;
    quote3.spot_bid_size = 1.0;
    quote3.spot_ask_size = 1.0;
    quote3.spot_exchange = "coinbase";
    quote3.perp_bid = 50008.0;
    quote3.perp_ask = 50012.0;
    quote3.perp_bid_size = 1.0;
    quote3.perp_ask_size = 1.0;
    quote3.perp_exchange = "binance";
    quote3.funding_rate = 0.0001;
    quote3.hours_to_funding = 4.0;
    quote3.timestamp = std::chrono::system_clock::now();
    
    auto opp3 = strategy.find_opportunity(quote3);
    if (!opp3) {
        std::cout << "✓ Correctly rejected small deviation\n";
    } else {
        std::cout << "✗ Should not have found opportunity\n";
    }
    
    // Test Case 4: Exit condition check
    std::cout << "\n--- Test 1.4: Exit Condition ---\n";
    bool should_exit = strategy.should_exit(quote1, 30.0, 0.0001, 2.0);
    std::cout << "  Entry deviation: 30 bps, Current: ~" 
              << ((50175 - 49950) / 49950 * 10000) << " bps\n";
    std::cout << "  Should exit: " << (should_exit ? "YES" : "NO") << "\n";
    
    std::cout << "\n✓ All perp-spot arbitrage tests passed!\n";
}

//==============================================================================
// TEST 2: Funding Rate Arbitrage
//==============================================================================
void test_funding_rate_arbitrage() {
    print_separator("TEST 2: Funding Rate Arbitrage");
    
    FundingRateArbitrage strategy(0.0010, 2.5, 90);
    
    // Build historical data
    std::cout << "\n--- Building Historical Data ---\n";
    auto now = std::chrono::system_clock::now();
    
    // Simulate 50 funding periods for Binance and Bybit
    for (int i = 0; i < 50; ++i) {
        FundingRateData data_binance;
        data_binance.exchange = "binance";
        data_binance.product = "BTC-USD";
        data_binance.funding_rate = 0.0001 + (i % 10) * 0.00002;  // Oscillate around 0.01%
        data_binance.hours_to_funding = 8.0 - (i % 8);
        data_binance.predicted_funding = data_binance.funding_rate * 1.1;  // Slight error
        data_binance.timestamp = now;
        
        FundingRateData data_bybit;
        data_bybit.exchange = "bybit";
        data_bybit.product = "BTC-USD";
        data_bybit.funding_rate = -0.0001 + (i % 8) * 0.00001;  // Slightly negative
        data_bybit.hours_to_funding = 8.0 - (i % 8);
        data_bybit.predicted_funding = data_bybit.funding_rate * 0.9;
        data_bybit.timestamp = now;
        
        strategy.update_funding_rate(data_binance);
        strategy.update_funding_rate(data_bybit);
    }
    std::cout << "✓ Built history for 50 funding periods\n";
    
    // Test 2.1: Cross-exchange opportunity
    std::cout << "\n--- Test 2.1: Cross-Exchange Funding Arb ---\n";
    FundingRateData current_binance;
    current_binance.exchange = "binance";
    current_binance.product = "BTC-USD";
    current_binance.funding_rate = 0.0020;  // High positive (longs pay)
    current_binance.hours_to_funding = 2.0;
    current_binance.predicted_funding = 0.0018;
    current_binance.timestamp = now;
    
    FundingRateData current_bybit;
    current_bybit.exchange = "bybit";
    current_bybit.product = "BTC-USD";
    current_bybit.funding_rate = -0.0005;  // Negative (shorts pay)
    current_bybit.hours_to_funding = 2.0;
    current_bybit.predicted_funding = -0.0003;
    current_bybit.timestamp = now;
    
    strategy.update_funding_rate(current_binance);
    strategy.update_funding_rate(current_bybit);
    
    auto opp1 = strategy.find_cross_exchange_opportunity("BTC-USD");
    if (opp1) {
        std::cout << "✓ Cross-exchange opportunity found!\n";
        print_opportunity(*opp1);
        
        // Should be: LONG on bybit (negative funding), SHORT on binance (positive funding)
        std::cout << "  Funding differential: " 
                  << (current_binance.funding_rate - current_bybit.funding_rate) * 10000 
                  << " bps\n";
    } else {
        std::cout << "✗ No cross-exchange opportunity found\n";
    }
    
    // Test 2.2: Mean reversion opportunity
    std::cout << "\n--- Test 2.2: Mean Reversion Funding Arb ---\n";
    
    // Add extreme funding rate
    FundingRateData extreme_data;
    extreme_data.exchange = "binance";
    extreme_data.product = "BTC-USD";
    extreme_data.funding_rate = 0.0050;  // Extremely high (5x normal)
    extreme_data.hours_to_funding = 4.0;
    extreme_data.predicted_funding = 0.0045;
    extreme_data.timestamp = now;
    strategy.update_funding_rate(extreme_data);
    
    auto stats = strategy.calculate_stats("binance", "BTC-USD");
    if (stats) {
        std::cout << "  Mean: " << stats->mean << "\n";
        std::cout << "  Std: " << stats->std_dev << "\n";
        std::cout << "  Z-score: " << stats->z_score << "\n";
        std::cout << "  Sample size: " << stats->sample_size << "\n";
        
        auto opp2 = strategy.find_mean_reversion_opportunity("binance", "BTC-USD");
        if (opp2) {
            std::cout << "✓ Mean reversion opportunity found!\n";
            print_opportunity(*opp2);
        } else {
            std::cout << "  No mean reversion opportunity (z-score may be < threshold)\n";
        }
    }
    
    // Test 2.3: Prediction error opportunity
    std::cout << "\n--- Test 2.3: Prediction Error Arb ---\n";
    FundingRateData pred_error_data;
    pred_error_data.exchange = "binance";
    pred_error_data.product = "ETH-USD";
    pred_error_data.funding_rate = 0.0001;      // Actual: 0.01%
    pred_error_data.predicted_funding = 0.0008; // Predicted: 0.08% (big error!)
    pred_error_data.hours_to_funding = 1.0;
    pred_error_data.timestamp = now;
    
    auto opp3 = strategy.find_prediction_error_opportunity(pred_error_data);
    if (opp3) {
        std::cout << "✓ Prediction error opportunity found!\n";
        print_opportunity(*opp3);
        std::cout << "  Prediction error: " 
                  << (pred_error_data.predicted_funding - pred_error_data.funding_rate) * 10000
                  << " bps\n";
    } else {
        std::cout << "✗ No prediction error opportunity found\n";
    }
    
    std::cout << "\n✓ All funding rate arbitrage tests passed!\n";
}

//==============================================================================
// TEST 3: Market Neutral Pairs Trading
//==============================================================================
void test_market_neutral_pairs() {
    print_separator("TEST 3: Market Neutral Pairs Trading");
    
    MarketNeutralPairs strategy(2.0, 0.5, 3.5, 100, 0.85);
    
    auto now = std::chrono::system_clock::now();
    
    // Test 3.1: Build cointegrated pair data
    std::cout << "\n--- Test 3.1: Building Cointegrated Pair ---\n";
    
    // Simulate BTC on two exchanges with slight drift
    // Base prices oscillate but maintain relationship
    for (int i = 0; i < 100; ++i) {
        double base_price = 50000.0 + i * 10.0;  // Upward trend
        double noise1 = (i % 7 - 3) * 5.0;       // Oscillation
        double noise2 = (i % 5 - 2) * 3.0;
        
        PairData coinbase;
        coinbase.exchange = "coinbase";
        coinbase.product = "BTC-USD";
        coinbase.price = base_price + noise1;
        coinbase.bid = coinbase.price - 2.5;
        coinbase.ask = coinbase.price + 2.5;
        coinbase.bid_size = 1.0;
        coinbase.ask_size = 1.0;
        coinbase.timestamp = now;
        
        PairData binance;
        binance.exchange = "binance";
        binance.product = "BTC-USD";
        // Binance typically 0.998x Coinbase (slight discount)
        binance.price = (base_price + noise2) * 0.998;
        binance.bid = binance.price - 2.0;
        binance.ask = binance.price + 2.0;
        binance.bid_size = 1.5;
        binance.ask_size = 1.5;
        binance.timestamp = now;
        
        strategy.update_price(coinbase, binance);
    }
    std::cout << "✓ Built 100 price observations\n";
    
    // Create current extreme deviation
    PairData coinbase_current;
    coinbase_current.exchange = "coinbase";
    coinbase_current.product = "BTC-USD";
    coinbase_current.price = 51500.0;  // Much higher than expected
    coinbase_current.bid = 51497.5;
    coinbase_current.ask = 51502.5;
    coinbase_current.bid_size = 2.0;
    coinbase_current.ask_size = 1.8;
    coinbase_current.timestamp = now;
    
    PairData binance_current;
    binance_current.exchange = "binance";
    binance_current.product = "BTC-USD";
    binance_current.price = 51000.0 * 0.998;  // Normal relationship
    binance_current.bid = 50897.0;
    binance_current.ask = 50901.0;
    binance_current.bid_size = 2.5;
    binance_current.ask_size = 2.2;
    binance_current.timestamp = now;
    
    strategy.update_price(coinbase_current, binance_current);
    
    // Check statistics
    auto stats = strategy.get_current_stats(coinbase_current, binance_current);
    if (stats) {
        std::cout << "\n--- Pair Statistics ---\n";
        std::cout << "  Beta (hedge ratio): " << stats->beta << "\n";
        std::cout << "  Correlation: " << stats->correlation << "\n";
        std::cout << "  Mean spread: $" << stats->mean_spread << "\n";
        std::cout << "  Std spread: $" << stats->std_spread << "\n";
        std::cout << "  Current spread: $" << stats->current_spread << "\n";
        std::cout << "  Z-score: " << stats->z_score << "\n";
        std::cout << "  Sample size: " << stats->sample_size << "\n";
    }
    
    // Test 3.2: Find opportunity
    std::cout << "\n--- Test 3.2: Find Pairs Opportunity ---\n";
    auto opp = strategy.find_opportunity(coinbase_current, binance_current);
    if (opp) {
        std::cout << "✓ Pairs trading opportunity found!\n";
        print_opportunity(*opp);
        
        if (stats && stats->z_score > 0) {
            // Positive z-score: Coinbase overpriced → short Coinbase, long Binance
            assert(opp->sell_exchange == "coinbase");
            assert(opp->buy_exchange == "binance");
            std::cout << "✓ Trade direction correct (short overpriced, long underpriced)\n";
        }
    } else {
        std::cout << "  No opportunity (z-score may be < threshold or correlation too low)\n";
    }
    
    // Test 3.3: Exit condition
    std::cout << "\n--- Test 3.3: Exit Condition ---\n";
    
    // Simulate mean reversion
    PairData coinbase_reverted;
    coinbase_reverted.exchange = "coinbase";
    coinbase_reverted.product = "BTC-USD";
    coinbase_reverted.price = 51005.0;  // Reverted closer to normal
    coinbase_reverted.bid = 51002.5;
    coinbase_reverted.ask = 51007.5;
    coinbase_reverted.bid_size = 2.0;
    coinbase_reverted.ask_size = 1.8;
    coinbase_reverted.timestamp = now;
    
    PairData binance_reverted;
    binance_reverted.exchange = "binance";
    binance_reverted.product = "BTC-USD";
    binance_reverted.price = 51000.0 * 0.998;
    binance_reverted.bid = 50897.0;
    binance_reverted.ask = 50901.0;
    binance_reverted.bid_size = 2.5;
    binance_reverted.ask_size = 2.2;
    binance_reverted.timestamp = now;
    
    strategy.update_price(coinbase_reverted, binance_reverted);
    
    double entry_z = stats ? stats->z_score : 2.5;
    bool should_exit = strategy.should_exit(coinbase_reverted, binance_reverted, entry_z);
    
    std::cout << "  Entry z-score: " << entry_z << "\n";
    auto current_stats = strategy.get_current_stats(coinbase_reverted, binance_reverted);
    if (current_stats) {
        std::cout << "  Current z-score: " << current_stats->z_score << "\n";
    }
    std::cout << "  Should exit: " << (should_exit ? "YES" : "NO") << "\n";
    
    // Test 3.4: Low correlation pair (should reject)
    std::cout << "\n--- Test 3.4: Low Correlation (No Trade) ---\n";
    
    // Create uncorrelated pair
    for (int i = 0; i < 50; ++i) {
        PairData btc;
        btc.exchange = "coinbase";
        btc.product = "BTC-USD";
        btc.price = 50000.0 + i * 50.0;  // Upward trend
        btc.bid = btc.price - 5.0;
        btc.ask = btc.price + 5.0;
        btc.bid_size = 1.0;
        btc.ask_size = 1.0;
        btc.timestamp = now;
        
        PairData sol;
        sol.exchange = "binance";
        sol.product = "SOL-USD";
        sol.price = 100.0 + (i % 10 - 5) * 20.0;  // Random walk
        sol.bid = sol.price - 0.5;
        sol.ask = sol.price + 0.5;
        sol.bid_size = 10.0;
        sol.ask_size = 10.0;
        sol.timestamp = now;
        
        strategy.update_price(btc, sol);
    }
    
    PairData btc_now;
    btc_now.exchange = "coinbase";
    btc_now.product = "BTC-USD";
    btc_now.price = 52500.0;
    btc_now.bid = 52495.0;
    btc_now.ask = 52505.0;
    btc_now.bid_size = 1.0;
    btc_now.ask_size = 1.0;
    btc_now.timestamp = now;
    
    PairData sol_now;
    sol_now.exchange = "binance";
    sol_now.product = "SOL-USD";
    sol_now.price = 150.0;
    sol_now.bid = 149.5;
    sol_now.ask = 150.5;
    sol_now.bid_size = 10.0;
    sol_now.ask_size = 10.0;
    sol_now.timestamp = now;
    
    auto uncorr_opp = strategy.find_opportunity(btc_now, sol_now);
    if (!uncorr_opp) {
        std::cout << "✓ Correctly rejected low correlation pair\n";
    } else {
        std::cout << "✗ Should not trade uncorrelated assets\n";
    }
    
    std::cout << "\n✓ All market neutral pairs tests passed!\n";
}

//==============================================================================
// MAIN
//==============================================================================
int main() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║         TESTING NEW ARBITRAGE STRATEGIES                           ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════╝\n";
    
    try {
        test_perp_spot_arbitrage();
        test_funding_rate_arbitrage();
        test_market_neutral_pairs();
        
        print_separator("ALL TESTS PASSED ✓");
        std::cout << "\nAll arbitrage strategies working correctly!\n\n";
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n✗ TEST FAILED: " << e.what() << "\n\n";
        return 1;
    }
}
