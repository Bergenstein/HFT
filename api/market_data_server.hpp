//==============================================================================
// HFT System - Market Data API Server
// Provides blackbox API for external strategy binaries
//==============================================================================

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <zmq.hpp>
#include <nlohmann/json.hpp>

namespace hft {
namespace api {

using json = nlohmann::json;

//==============================================================================
// Market Data Snapshot
//==============================================================================
struct MarketDataSnapshot {
    std::string exchange;
    std::string symbol;
    uint64_t timestamp_ns;
    double bid_price;
    double ask_price;
    double bid_qty;
    double ask_qty;
    double funding_rate;          // 8-hour rate
    double funding_rate_annual;   // APY
    
    json to_json() const {
        return {
            {"exchange", exchange},
            {"symbol", symbol},
            {"timestamp_ns", timestamp_ns},
            {"bid_price", bid_price},
            {"ask_price", ask_price},
            {"bid_qty", bid_qty},
            {"ask_qty", ask_qty},
            {"funding_rate", funding_rate},
            {"funding_rate_annual", funding_rate_annual}
        };
    }
    
    static MarketDataSnapshot from_json(const json& j) {
        return {
            j["exchange"].get<std::string>(),
            j["symbol"].get<std::string>(),
            j["timestamp_ns"].get<uint64_t>(),
            j["bid_price"].get<double>(),
            j["ask_price"].get<double>(),
            j["bid_qty"].get<double>(),
            j["ask_qty"].get<double>(),
            j.value("funding_rate", 0.0),
            j.value("funding_rate_annual", 0.0)
        };
    }
};

//==============================================================================
// Trading Signal (from strategy to system)
//==============================================================================
struct TradingSignal {
    std::string strategy_id;
    std::string symbol;
    std::string action;  // "BUY", "SELL", "HOLD", "LONG", "SHORT", "CLOSE"
    double quantity;
    double price;        // 0 for market order
    std::string exchange;
    uint64_t timestamp_ns;
    json metadata;       // Optional strategy-specific data
    
    json to_json() const {
        return {
            {"strategy_id", strategy_id},
            {"symbol", symbol},
            {"action", action},
            {"quantity", quantity},
            {"price", price},
            {"exchange", exchange},
            {"timestamp_ns", timestamp_ns},
            {"metadata", metadata}
        };
    }
    
    static TradingSignal from_json(const json& j) {
        return {
            j["strategy_id"].get<std::string>(),
            j["symbol"].get<std::string>(),
            j["action"].get<std::string>(),
            j["quantity"].get<double>(),
            j.value("price", 0.0),
            j.value("exchange", ""),
            j["timestamp_ns"].get<uint64_t>(),
            j.value("metadata", json::object())
        };
    }
};

//==============================================================================
// Market Data Server
// Publishes market data via ZeroMQ PUB socket on tcp://*:5555
// Receives signals via ZeroMQ PULL socket on tcp://*:5556
//==============================================================================
class MarketDataServer {
public:
    MarketDataServer(const std::string& pub_endpoint = "tcp://*:5555",
                     const std::string& signal_endpoint = "tcp://*:5556")
        : pub_endpoint_(pub_endpoint)
        , signal_endpoint_(signal_endpoint)
        , running_(false)
    {}
    
    ~MarketDataServer() {
        stop();
    }
    
    void start() {
        if (running_.load()) return;
        
        running_.store(true);
        
        // Publisher thread for market data
        pub_thread_ = std::thread([this]() {
            zmq::context_t context(1);
            zmq::socket_t publisher(context, zmq::socket_type::pub);
            publisher.bind(pub_endpoint_);
            
            while (running_.load()) {
                // Process pending market data
                std::lock_guard<std::mutex> lock(queue_mutex_);
                while (!market_data_queue_.empty()) {
                    auto& data = market_data_queue_.front();
                    json j = data.to_json();
                    std::string msg = j.dump();
                    
                    zmq::message_t message(msg.size());
                    memcpy(message.data(), msg.data(), msg.size());
                    publisher.send(message, zmq::send_flags::dontwait);
                    
                    market_data_queue_.pop();
                }
            }
        });
        
        // Signal receiver thread
        signal_thread_ = std::thread([this]() {
            zmq::context_t context(1);
            zmq::socket_t receiver(context, zmq::socket_type::pull);
            receiver.bind(signal_endpoint_);
            
            while (running_.load()) {
                zmq::message_t message;
                auto result = receiver.recv(message, zmq::recv_flags::dontwait);
                
                if (result) {
                    std::string msg(static_cast<char*>(message.data()), message.size());
                    try {
                        json j = json::parse(msg);
                        TradingSignal signal = TradingSignal::from_json(j);
                        
                        // Call registered signal handler
                        if (signal_handler_) {
                            signal_handler_(signal);
                        }
                    } catch (const std::exception& e) {
                        // Log error
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }
    
    void stop() {
        if (!running_.load()) return;
        running_.store(false);
        
        if (pub_thread_.joinable()) pub_thread_.join();
        if (signal_thread_.joinable()) signal_thread_.join();
    }
    
    // Publish market data (called by HFT system)
    void publish(const MarketDataSnapshot& data) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        market_data_queue_.push(data);
    }
    
    // Register callback for trading signals from strategies
    void on_signal(std::function<void(const TradingSignal&)> handler) {
        signal_handler_ = handler;
    }
    
private:
    std::string pub_endpoint_;
    std::string signal_endpoint_;
    std::atomic<bool> running_;
    std::thread pub_thread_;
    std::thread signal_thread_;
    std::mutex queue_mutex_;
    std::queue<MarketDataSnapshot> market_data_queue_;
    std::function<void(const TradingSignal&)> signal_handler_;
};

} // namespace api
} // namespace hft
