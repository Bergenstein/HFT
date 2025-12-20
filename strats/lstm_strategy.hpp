#pragma once

/**
 * LSTM Trading Strategy
 * 
 * Integrates the trained LSTM model into the hot-path trading system.
 * Uses memory-mapped weights for fast loading and cache-optimized inference.
 * 
 * Cold Path: Model training (Python)
 * Hot Path: Real-time inference (C++)
 * 
 * Architecture:
 *   OrderBook Update -> Feature Extractor -> LSTM Inference -> Signal
 */

#include "../bt/backtester.hpp"
#include "../core/order_book.hpp"
#include "../ml/lstm_inference.hpp"
#include "../ml/feature_extractor.hpp"
#include <string>
#include <memory>

/**
 * LSTM Strategy for HFT
 * 
 * Uses trained LSTM model to predict short-term price movements
 * and generate trading signals.
 * 
 * SIGNAL INTERPRETATION:
 *   Prediction > buy_threshold  -> BUY (+1)
 *   Prediction < sell_threshold -> SELL (-1)
 *   Otherwise                   -> HOLD (0)
 * 
 * POSITION MANAGEMENT:
 *   - Enters position on strong signal
 *   - Holds for configurable number of ticks
 *   - Exits on opposite signal or timeout
 */
class LSTMStrategy : public Strategy {
public:
    /**
     * Constructor
     * 
     * @param model_dir: Directory containing exported model files
     * @param model_name: Base name of model files (e.g., "lstm_trading_simple")
     * @param hold_ticks: Maximum ticks to hold position
     * @param buy_threshold: Threshold for buy signal (basis points)
     * @param sell_threshold: Threshold for sell signal (basis points)
     */
    LSTMStrategy(
        const std::string& model_dir = "",
        const std::string& model_name = "lstm_trading_simple",
        int hold_ticks = 100,
        double buy_threshold = 5.0,
        double sell_threshold = -5.0
    ) : hold_ticks_(hold_ticks),
        buy_threshold_(buy_threshold),
        sell_threshold_(sell_threshold) {
        
        if (!model_dir.empty()) {
            load_model(model_dir, model_name);
        }
    }
    
    /**
     * Load LSTM model from files
     */
    void load_model(const std::string& model_dir, const std::string& model_name) {
        std::string weights_path = model_dir + "/" + model_name + ".bin";
        std::string config_path = model_dir + "/" + model_name + ".json";
        
        lstm_.load(weights_path, config_path);
        model_loaded_ = lstm_.is_loaded();
        
        if (model_loaded_) {
            // Update thresholds from model config
            buy_threshold_ = lstm_.config().buy_threshold;
            sell_threshold_ = lstm_.config().sell_threshold;
        }
    }
    
    /**
     * Check if model is ready
     */
    bool is_ready() const {
        return model_loaded_ && feature_extractor_.ready();
    }
    
    /**
     * Main strategy callback - called on every tick
     * 
     * @param tc: Tick context with timestamp and price info
     * @param book: Current order book state
     * @return: Trading signal (+1 buy, -1 sell, 0 hold)
     */
    int on_tick(const TickContext& tc, const core::OrderBook& book) override {
        if (!model_loaded_) {
            return 0;  // Model not loaded, no signal
        }
        
        // Update feature extractor with new book state
        feature_extractor_.on_update(book);
        
        // Check if we have enough data
        if (!feature_extractor_.ready()) {
            return 0;  // Not enough history yet
        }
        
        // Extract features
        float features[ml::FeatureExtractor::NUM_FEATURES];
        if (!feature_extractor_.get_features(features)) {
            return 0;
        }
        
        // Update LSTM with features
        lstm_.update_features(features);
        
        // Get prediction
        auto [signal, prediction] = lstm_.get_signal_with_prediction();
        last_prediction_ = prediction;
        
        // Position management
        if (ticks_left_ > 0) {
            --ticks_left_;
        }
        
        // Currently in a position
        if (state_ != 0) {
            // Exit on timeout
            if (ticks_left_ == 0) {
                int exit_signal = -state_;
                state_ = 0;
                return exit_signal;
            }
            
            // Exit or flip on opposite signal
            if (state_ > 0 && signal < 0) {
                // Was long, now sell signal
                state_ = -1;
                ticks_left_ = hold_ticks_;
                return -1;  // Sell
            } else if (state_ < 0 && signal > 0) {
                // Was short, now buy signal
                state_ = 1;
                ticks_left_ = hold_ticks_;
                return 1;  // Buy
            }
            
            return 0;  // Hold current position
        }
        
        // Not in a position - check for entry
        if (signal != 0) {
            state_ = signal;
            ticks_left_ = hold_ticks_;
            return signal;
        }
        
        return 0;  // No signal
    }
    
    /**
     * Get last prediction value (for debugging/logging)
     */
    double last_prediction() const { return last_prediction_; }
    
    /**
     * Get current position state
     */
    int position_state() const { return state_; }
    
    /**
     * Reset strategy state
     */
    void reset() {
        state_ = 0;
        ticks_left_ = 0;
        last_prediction_ = 0.0;
        feature_extractor_.reset();
        lstm_.reset();
    }

private:
    // Model
    ml::LSTMInference lstm_;
    ml::FeatureExtractor feature_extractor_;
    bool model_loaded_ = false;
    
    // Parameters
    int hold_ticks_;
    double buy_threshold_;
    double sell_threshold_;
    
    // State
    int state_ = 0;  // Current position: +1 long, -1 short, 0 flat
    int ticks_left_ = 0;
    double last_prediction_ = 0.0;
};


/**
 * Factory function to create LSTM strategy
 */
inline std::unique_ptr<Strategy> create_lstm_strategy(
    const std::string& model_dir,
    const std::string& model_name = "lstm_trading_simple",
    int hold_ticks = 100
) {
    return std::make_unique<LSTMStrategy>(model_dir, model_name, hold_ticks);
}
