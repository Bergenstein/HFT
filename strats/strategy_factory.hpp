#pragma once
#include <memory>        // For std::unique_ptr
#include <string>        // For strategy names
#include <stdexcept>     // For std::runtime_error
#include "../bt/backtester.hpp"  // Base Strategy class
// Strategy implementations
#include "imbalance_taker.hpp"
#include "multi_imbalance_taker.hpp"
#include "strategy_ofi.hpp"
#include "microprice_strategy.hpp"
#include "quote_intensity_strategy.hpp"
#include "spread_reversion_strategy.hpp"
#include "vpin_strategy.hpp"
#include "volume_weighted_spread_strategy.hpp"

//=============================================================================
// STRATEGY FACTORY: Centralized Strategy Creation
//=============================================================================
// 
// PURPOSE:
// Factory pattern for creating strategy instances from configuration.
// Decouples strategy creation from strategy usage.
//
// DESIGN PATTERN: Factory Method
// - Client code doesn't need to know strategy constructors
// - Easy to add new strategies (just update factory)
// - Consistent interface for all strategy creation
// - Enables configuration-driven strategy selection
//
// WHY FACTORY PATTERN?
// 1. Configuration flexibility: Read strategy from config file
// 2. Runtime selection: Choose strategy based on market conditions
// 3. Testing: Easy to test all strategies with same interface
// 4. Backtesting: Run grid search over all strategies
//
// EXAMPLE USAGE:
// // From config file: strategy_name="imbalance", threshold=0.6
// auto strat = StrategyFactory::create("imbalance", 0.6, 5.0);
// // Returns std::unique_ptr<ImbalanceTaker>
//
// PARAMETER CONVENTION:
// - param1: Primary strategy parameter (threshold, window, etc.)
// - param2: Secondary parameter, often hold time in SECONDS
//   - Converted to ticks: hold_ticks = param2 * 20 (assuming 20 ticks/sec)
//
// MEMORY MANAGEMENT:
// - Returns std::unique_ptr (ownership transfer)
// - Caller is responsible for strategy lifetime
// - Strategy automatically deleted when unique_ptr goes out of scope
//
//=============================================================================
class StrategyFactory {
public:
    //=========================================================================
    // CREATE: Factory Method to Instantiate Strategies
    //=========================================================================
    // name: Strategy name (e.g., "imbalance", "ofi", "microprice")
    // param1: Primary parameter (threshold, window size, etc.)
    // param2: Secondary parameter (often hold time in seconds, default 0.0)
    // Returns: unique_ptr to Strategy (polymorphic base class)
    //
    // PARAMETER MAPPING:
    // Each strategy has different constructor parameters
    // Factory normalizes this to (name, param1, param2) interface
    //
    // TICK CONVERSION:
    // - param2 is in SECONDS (user-friendly)
    // - Strategies expect TICKS (internal representation)
    // - Conversion: ticks = seconds * 20 (assuming 50ms per tick)
    // - Example: param2=5.0 (5 seconds) → 100 ticks
    //
    // ERROR HANDLING:
    // - Unknown strategy name throws std::runtime_error
    // - Client should catch and handle gracefully
    static std::unique_ptr<Strategy> create(const std::string& name, double param1, double param2 = 0.0) {
        //=====================================================================
        // IMBALANCE TAKER STRATEGY
        //=====================================================================
        // param1: Imbalance threshold (default: 0.6 = 60%)
        // param2: Hold time in seconds (converted to ticks)
        //
        // EXAMPLE:
        // create("imbalance", 0.6, 7.5)
        // → ImbalanceTaker(threshold=0.6, hold_ticks=150)
        if (name == "imbalance" || name == "imbalance_taker") {
            // Convert seconds to ticks: ~20 ticks/sec
            int hold_ticks = (param2 > 0) ? static_cast<int>(param2 * 20) : 150;
            return std::make_unique<ImbalanceTaker>(param1, hold_ticks);
        }
        //=====================================================================
        // MULTI-IMBALANCE TAKER STRATEGY
        //=====================================================================
        // Same as ImbalanceTaker but handles multiple products
        // param1: Imbalance threshold
        // param2: Hold time in seconds
        else if (name == "multi_imbalance") {
            int hold_ticks = (param2 > 0) ? static_cast<int>(param2 * 20) : 150;
            return std::make_unique<MultiImbalanceTaker>(param1, hold_ticks);
        }
        //=====================================================================
        // ORDER FLOW IMBALANCE (OFI) STRATEGY
        //=====================================================================
        // param1: Entry threshold (default: 0.15)
        // param2: Exit threshold (default: 0.07)
        //
        // OFI-SPECIFIC PARAMETERS:
        // - decay: Exponential decay factor (0.97 = slow decay)
        // - max_pos: Maximum position size (5)
        // - cooldown: Ticks to wait before re-entry (3)
        //
        // EXAMPLE:
        // create("ofi", 0.20, 0.10)
        // → ROFIStrategy with custom entry/exit thresholds
        else if (name == "ofi" || name == "rofi") {
            ROFIStrategy::Params p;
            p.decay = 0.97;          // How fast OFI decays (higher = slower)
            p.enter_thr = param1 > 0 ? param1 : 0.15;  // Threshold to enter
            p.exit_thr = param2 > 0 ? param2 : 0.07;   // Threshold to exit
            p.max_pos = 5;           // Position limit
            p.cooldown = 3;          // Ticks before re-entry
            return std::make_unique<ROFIStrategy>(p);
        }
        //=====================================================================
        // MICROPRICE STRATEGY
        //=====================================================================
        // param1: Deviation threshold (e.g., 0.0002 = 2 basis points)
        // param2: Hold time in seconds
        //
        // FIXED PARAMETERS:
        // - spread_multiple: 3 (entry at mid ± 3*spread)
        //
        // EXAMPLE:
        // create("microprice", 0.0003, 5.0)
        // → MicropriceStrategy(threshold=0.0003, hold_ticks=100, spread_mult=3)
        else if (name == "microprice") {
            int hold_ticks = (param2 > 0) ? static_cast<int>(param2 * 20) : 100;
            return std::make_unique<MicropriceStrategy>(param1, hold_ticks, 3);
        }
        //=====================================================================
        // QUOTE INTENSITY STRATEGY
        //=====================================================================
        // param1: Intensity imbalance threshold (default: 0.4 = 40%)
        // param2: Hold time in seconds
        //
        // FIXED PARAMETERS:
        // - window: 100 ticks (lookback period for intensity)
        //
        // EXAMPLE:
        // create("quote_intensity", 0.35, 2.5)
        // → QuoteIntensityStrategy(window=100, threshold=0.35, hold_ticks=50)
        else if (name == "quote_intensity") {
            int hold_ticks = (param2 > 0) ? static_cast<int>(param2 * 20) : 50;
            return std::make_unique<QuoteIntensityStrategy>(100, param1, hold_ticks);
        }
        //=====================================================================
        // SPREAD REVERSION STRATEGY
        //=====================================================================
        // param1: Spread multiplier threshold (default: 1.5 = 50% wider)
        // param2: Hold time in seconds
        //
        // FIXED PARAMETERS:
        // - window: 50 ticks (for average spread calculation)
        //
        // EXAMPLE:
        // create("spread_reversion", 1.6, 5.0)
        // → SpreadReversionStrategy(window=50, threshold=1.6, hold_ticks=100)
        else if (name == "spread_reversion") {
            int hold_ticks = (param2 > 0) ? static_cast<int>(param2 * 20) : 100;
            return std::make_unique<SpreadReversionStrategy>(50, param1, hold_ticks);
        }
        //=====================================================================
        // VPIN STRATEGY
        //=====================================================================
        // param1: Low VPIN threshold to ENTER trades (default: 0.3)
        // param2: Hold time in seconds
        //
        // FIXED PARAMETERS:
        // - bucket_volume: 50.0 (volume per bucket)
        // - num_buckets: 50 (rolling window of buckets)
        // - high_threshold: 0.7 (VPIN to EXIT trades)
        //
        // EXAMPLE:
        // create("vpin", 0.25, 5.0)
        // → VPINStrategy(bucket_vol=50, num_buckets=50, low_thr=0.25, high_thr=0.7, hold=100)
        else if (name == "vpin") {
            int hold_ticks = (param2 > 0) ? static_cast<int>(param2 * 20) : 100;
            return std::make_unique<VPINStrategy>(50.0, 50, param1, 0.7, hold_ticks);
        }
        //=====================================================================
        // VOLUME-WEIGHTED SPREAD STRATEGY
        //=====================================================================
        // param1: Deviation threshold (e.g., 0.002 = 0.2%)
        // param2: Hold time in seconds
        //
        // FIXED PARAMETERS:
        // - depth_levels: 5 (number of order book levels to include)
        //
        // EXAMPLE:
        // create("vw_spread", 0.0025, 4.0)
        // → VolumeWeightedSpreadStrategy(depth=5, threshold=0.0025, hold_ticks=80)
        else if (name == "volume_weighted_spread" || name == "vw_spread") {
            int hold_ticks = (param2 > 0) ? static_cast<int>(param2 * 20) : 80;
            return std::make_unique<VolumeWeightedSpreadStrategy>(5, param1, hold_ticks);
        }
        //=====================================================================
        // UNKNOWN STRATEGY
        //=====================================================================
        // Throw exception for invalid strategy name
        // Client should catch and display error to user
        else {
            throw std::runtime_error("Unknown strategy: " + name);
        }
    }

    //=========================================================================
    // PRINT_AVAILABLE_STRATEGIES: List All Supported Strategies
    //=========================================================================
    // Useful for CLI help, documentation, config validation
    // Prints to stdout with human-readable descriptions
    static void print_available_strategies() {
        std::cout << "Available Strategies:\n";
        std::cout << "  imbalance          - Order book imbalance taker\n";
        std::cout << "  ofi                - Order Flow Imbalance (ROFI)\n";
        std::cout << "  microprice         - Microprice mean reversion\n";
        std::cout << "  quote_intensity    - Quote intensity imbalance\n";
        std::cout << "  spread_reversion   - Spread reversion strategy\n";
        std::cout << "  vpin               - Volume-synchronized toxicity\n";
        std::cout << "  vw_spread          - Volume-weighted spread\n";
    }
};

//=============================================================================
// USAGE EXAMPLES
//=============================================================================
//
// 1. BASIC USAGE:
// auto strat = StrategyFactory::create("imbalance", 0.6);
// // Use default hold_ticks (150)
//
// 2. WITH CUSTOM HOLD TIME:
// auto strat = StrategyFactory::create("microprice", 0.0003, 5.0);
// // 5 seconds = 100 ticks
//
// 3. ERROR HANDLING:
// try {
//     auto strat = StrategyFactory::create("unknown_strat", 0.5);
// } catch (const std::runtime_error& e) {
//     std::cerr << "Error: " << e.what() << "\n";
// }
//
// 4. CONFIGURATION-DRIVEN:
// struct Config {
//     std::string strategy_name;
//     double threshold;
//     double hold_seconds;
// };
// Config cfg = read_config("config.json");
// auto strat = StrategyFactory::create(cfg.strategy_name, cfg.threshold, cfg.hold_seconds);
//
// 5. GRID SEARCH (Backtesting):
// for (auto& name : {"imbalance", "microprice", "vpin"}) {
//     for (double thresh = 0.3; thresh <= 0.7; thresh += 0.1) {
//         auto strat = StrategyFactory::create(name, thresh);
//         double sharpe = backtest(strat.get(), data);
//         std::cout << name << " @ " << thresh << ": Sharpe=" << sharpe << "\n";
//     }
// }
//
//=============================================================================
// ADDING NEW STRATEGIES
//=============================================================================
//
// To add a new strategy to the factory:
//
// 1. Include the header:
//    #include "my_new_strategy.hpp"
//
// 2. Add else-if branch in create():
//    else if (name == "my_strategy") {
//        return std::make_unique<MyNewStrategy>(param1, param2);
//    }
//
// 3. Update print_available_strategies():
//    std::cout << "  my_strategy        - Description\n";
//
// 4. Document parameter mapping in comments
//
//=============================================================================
// DESIGN CONSIDERATIONS
//=============================================================================
//
// PROS:
// - Centralized: All strategy creation in one place
// - Flexible: Easy to add/remove strategies
// - Testable: Can mock factory for unit tests
// - Configurable: Read from file, CLI, environment
//
// CONS:
// - Coupling: Factory depends on all strategy headers
// - Limited: Only 2 parameters (can extend to variadic template)
// - Type erasure: Returns base Strategy*, loses type information
//
// ALTERNATIVES:
// 1. Registry pattern: Strategies register themselves at startup
// 2. Plugin system: Load strategies from shared libraries (.so/.dll)
// 3. Template-based: Compile-time factory using templates
// 4. Builder pattern: Fluent interface for complex strategies
//
// CHOICE: Simple factory is sufficient for this HFT system
// - Small number of strategies (< 10)
// - Parameters are simple (1-2 doubles)
// - No need for runtime plugin loading
//
//=============================================================================
