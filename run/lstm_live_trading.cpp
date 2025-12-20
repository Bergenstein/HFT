/**
 * LSTM Live Trading Integration Example
 */

#include <iostream>
#include <random>
#include <chrono>

#include "../core/order_book.hpp"
#include "../ml/lstm_inference.hpp"
#include "../ml/feature_extractor.hpp"

using namespace std::chrono;

class LSTMTradingEngine {
public:
    LSTMTradingEngine(const std::string& model_dir, const std::string& model_name) {
        std::string weights_path = model_dir + "/" + model_name + ".bin";
        std::string config_path = model_dir + "/" + model_name + ".json";
        
        lstm_.load(weights_path, config_path);
        
        if (lstm_.is_loaded()) {
            std::cout << "Model loaded!\n";
            std::cout << "  Input features: " << lstm_.config().input_size << "\n";
            std::cout << "  Sequence length: " << lstm_.config().sequence_length << "\n";
        }
    }
    
    int on_orderbook_update(const core::OrderBook& book) {
        feature_extractor_.on_update(book);
        
        if (!feature_extractor_.ready()) return 0;
        
        float features[ml::FeatureExtractor::NUM_FEATURES];
        if (!feature_extractor_.get_features(features)) return 0;
        
        lstm_.update_features(features);
        auto [signal, prediction] = lstm_.get_signal_with_prediction();
        
        ++tick_count_;
        last_prediction_ = prediction;
        
        if (signal != 0 && position_ != signal) {
            position_ = signal;
            ++trade_count_;
            return signal;
        }
        return 0;
    }
    
    size_t tick_count() const { return tick_count_; }
    size_t trade_count() const { return trade_count_; }
    float last_prediction() const { return last_prediction_; }
    
private:
    ml::LSTMInference lstm_;
    ml::FeatureExtractor feature_extractor_;
    int position_ = 0;
    size_t tick_count_ = 0;
    size_t trade_count_ = 0;
    float last_prediction_ = 0.0f;
};

int main() {
    std::cout << "LSTM Live Trading Demo\n\n";
    
    try {
        LSTMTradingEngine engine("../ml/exported", "lstm_trading_simple");
        
        core::OrderBook book;
        std::mt19937 rng(42);
        std::normal_distribution<double> price_dist(0.0, 0.01);
        std::uniform_real_distribution<double> qty_dist(10.0, 100.0);
        
        double base_price = 100.0;
        const int NUM_TICKS = 100000;
        
        auto start = high_resolution_clock::now();
        
        for (int i = 0; i < NUM_TICKS; ++i) {
            base_price += price_dist(rng);
            book.set_level(true, base_price - 0.01, qty_dist(rng));
            book.set_level(false, base_price + 0.01, qty_dist(rng));
            engine.on_orderbook_update(book);
        }
        
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<microseconds>(end - start).count();
        
        std::cout << "\nResults:\n";
        std::cout << "  Ticks: " << NUM_TICKS << "\n";
        std::cout << "  Time: " << duration << " us\n";
        std::cout << "  Latency: " << (duration * 1000 / NUM_TICKS) << " ns/tick\n";
        std::cout << "  Throughput: " << (NUM_TICKS * 1e6 / duration) << " ticks/sec\n";
        std::cout << "  Trades: " << engine.trade_count() << "\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
