// sim/market_replay.hpp - Replay historical market data for realistic simulation
#pragma once

#include "exchange_simulator.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

namespace sim {

// Market data replay engine - feeds historical data into simulator
//
// MarketReplay: injects historical NDJSON market events into ExchangeSimulator
//
// - It supports time scaling via speed_multiplier; 1.0 = original timings, 10.0 = 10x speed.
// - It filters by product_id so you can replay a single product into a multi-product simulation.
// - Replay injects a simplified form of order book updates using `exchange_.inject_market_data()`.
// - For more realistic replay: build multiple levels or add trades as simulated market orders
//   instead of only updating top-of-book levels.
//
// THREAD SAFE USAGE:
// - MarketReplay runs a background thread that calls inject_market_data(). That method
//   uses a per-product mutex in ExchangeSimulator to protect the underlying MatchingEngine.
// - If you run MarketReplay concurrently with other threads that call the engine (e.g., WebSocket
//   handlers, strategies submitting orders in real-time), the per-product mutex avoids races.
//
// USAGE NOTE:
// - If you need deterministic replay timings across test runs, ensure the replay data has consistent
//   timestamps and that system clock resolution is sufficient. For very high-fidelity testing, use
//   files recorded from the same system clock or translate timestamps to a stable monotonic scale.
class MarketReplay {
public:
    struct ReplayConfig {
        std::string data_file;
        std::string product_id;
        double speed_multiplier = 1.0;  // 1.0 = real-time, 10.0 = 10x speed
        bool loop = false;               // Loop replay when data ends
    };
    
    MarketReplay(ExchangeSimulator& exchange, const ReplayConfig& config)
        : exchange_(exchange), config_(config), running_(false) {
        load_data();
    }
    
    ~MarketReplay() {
        stop();
    }
    
    // Start replay
    void start() {
        if (running_) return;
        
        running_ = true;
        
        replay_thread_ = std::thread([this]() {
            replay_loop();
        });
        
        std::cout << "[MarketReplay] Started replaying " << config_.data_file 
                  << " (" << data_.size() << " events, " 
                  << config_.speed_multiplier << "x speed)\n";
    }
    
    // Stop replay
    void stop() {
        if (!running_) return;
        
        running_ = false;
        
        if (replay_thread_.joinable()) {
            replay_thread_.join();
        }
        
        std::cout << "[MarketReplay] Stopped\n";
    }
    
    // Get replay progress
    double get_progress() const {
        if (data_.empty()) return 0.0;
        return static_cast<double>(current_index_) / data_.size();
    }
    
private:
    void load_data() {
        std::ifstream file(config_.data_file);
        if (!file.is_open()) {
            std::cerr << "[MarketReplay] Failed to open " << config_.data_file << "\n";
            return;
        }
        
        std::string line;
        while (std::getline(file, line)) {
            try {
                json event = json::parse(line);
                
                // Filter by product_id if specified
                if (!config_.product_id.empty()) {
                    std::string product = event.value("product_id", "");
                    if (product != config_.product_id) continue;
                }
                
                data_.push_back(event);
                
            } catch (const std::exception& e) {
                std::cerr << "[MarketReplay] Parse error: " << e.what() << "\n";
            }
        }
        
        std::cout << "[MarketReplay] Loaded " << data_.size() << " events from " 
                  << config_.data_file << "\n";
    }
    
    void replay_loop() {
        current_index_ = 0;
        
        auto start_time = std::chrono::steady_clock::now();
        uint64_t first_timestamp = 0;
        
        while (running_) {
            if (current_index_ >= data_.size()) {
                if (config_.loop) {
                    current_index_ = 0;
                    start_time = std::chrono::steady_clock::now();
                    first_timestamp = 0;
                    continue;
                } else {
                    break;
                }
            }
            
            const auto& event = data_[current_index_];
            
            // Extract timestamp
            uint64_t event_timestamp = event.value("timestamp", 0ULL);
            
            if (first_timestamp == 0) {
                first_timestamp = event_timestamp;
            }
            
            // Calculate delay based on timestamp difference
            uint64_t elapsed_real_us = (event_timestamp - first_timestamp);
            uint64_t elapsed_sim_us = static_cast<uint64_t>(
                elapsed_real_us / config_.speed_multiplier
            );
            
            auto target_time = start_time + std::chrono::microseconds(elapsed_sim_us);
            auto now = std::chrono::steady_clock::now();
            
            if (target_time > now) {
                std::this_thread::sleep_until(target_time);
            }
            
            // Process event
            process_event(event);
            
            current_index_++;
        }
        
        std::cout << "[MarketReplay] Replay completed (" << current_index_ << " events)\n";
    }
    
    void process_event(const json& event) {
        std::string type = event.value("type", "");
        std::string product_id = event.value("product_id", "");
        
        if (type == "l2update" || type == "snapshot") {
            process_order_book_update(event);
        } else if (type == "ticker" || type == "match") {
            process_trade(event);
        }
    }
    
    void process_order_book_update(const json& event) {
        // Extract order book data and inject into simulator
        std::string product_id = event.value("product_id", "");
        
        if (event.contains("bids") && event.contains("asks")) {
            auto bids = event["bids"];
            auto asks = event["asks"];
            
            if (!bids.empty() && !asks.empty()) {
                double best_bid = std::stod(bids[0][0].get<std::string>());
                double best_ask = std::stod(asks[0][0].get<std::string>());
                double bid_size = std::stod(bids[0][1].get<std::string>());
                double ask_size = std::stod(asks[0][1].get<std::string>());
                
                exchange_.inject_market_data(product_id, best_bid, best_ask, bid_size, ask_size);
            }
        }
    }
    
    void process_trade(const json& event) {
        // Trade events can be used to update last traded price
        // or to create market orders in the simulator
    }
    
    ExchangeSimulator& exchange_;
    ReplayConfig config_;
    
    std::vector<json> data_;
    size_t current_index_ = 0;
    
    std::atomic<bool> running_;
    std::thread replay_thread_;
};

} // namespace sim
