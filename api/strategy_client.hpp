//==============================================================================
// Strategy Client Interface
// Used by external strategy binaries to receive market data and send signals
//==============================================================================

#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include "market_data_server.hpp"

namespace hft {
namespace api {

//==============================================================================
// Strategy Client
// Subscribes to market data from tcp://localhost:5555
// Sends trading signals to tcp://localhost:5556
//==============================================================================
class StrategyClient {
public:
    StrategyClient(const std::string& strategy_id,
                   const std::string& server_address = "tcp://localhost:5555",
                   const std::string& signal_address = "tcp://localhost:5556")
        : strategy_id_(strategy_id)
        , server_address_(server_address)
        , signal_address_(signal_address)
        , running_(false)
    {}
    
    ~StrategyClient() {
        stop();
    }
    
    void start() {
        if (running_.load()) return;
        running_.store(true);
        
        // Market data subscriber thread
        sub_thread_ = std::thread([this]() {
            zmq::context_t context(1);
            zmq::socket_t subscriber(context, zmq::socket_type::sub);
            subscriber.connect(server_address_);
            subscriber.set(zmq::sockopt::subscribe, ""); // Subscribe to all
            
            while (running_.load()) {
                zmq::message_t message;
                auto result = subscriber.recv(message, zmq::recv_flags::dontwait);
                
                if (result) {
                    std::string msg(static_cast<char*>(message.data()), message.size());
                    try {
                        json j = json::parse(msg);
                        MarketDataSnapshot data = MarketDataSnapshot::from_json(j);
                        
                        // Call registered handler
                        if (data_handler_) {
                            data_handler_(data);
                        }
                    } catch (const std::exception& e) {
                        // Log error
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        
        // Signal sender (lazy initialization)
        signal_context_ = std::make_unique<zmq::context_t>(1);
        signal_socket_ = std::make_unique<zmq::socket_t>(*signal_context_, zmq::socket_type::push);
        signal_socket_->connect(signal_address_);
    }
    
    void stop() {
        if (!running_.load()) return;
        running_.store(false);
        
        if (sub_thread_.joinable()) sub_thread_.join();
        signal_socket_.reset();
        signal_context_.reset();
    }
    
    // Register callback for market data
    void on_market_data(std::function<void(const MarketDataSnapshot&)> handler) {
        data_handler_ = handler;
    }
    
    // Send trading signal to system
    void send_signal(const TradingSignal& signal) {
        if (!signal_socket_) return;
        
        json j = signal.to_json();
        std::string msg = j.dump();
        
        zmq::message_t message(msg.size());
        memcpy(message.data(), msg.data(), msg.size());
        signal_socket_->send(message, zmq::send_flags::dontwait);
    }
    
    // Helper: Create signal with strategy ID
    TradingSignal create_signal(const std::string& symbol,
                                const std::string& action,
                                double quantity,
                                double price = 0.0,
                                const std::string& exchange = "") {
        return {
            strategy_id_,
            symbol,
            action,
            quantity,
            price,
            exchange,
            static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count()
            ),
            json::object()
        };
    }
    
private:
    std::string strategy_id_;
    std::string server_address_;
    std::string signal_address_;
    std::atomic<bool> running_;
    std::thread sub_thread_;
    std::unique_ptr<zmq::context_t> signal_context_;
    std::unique_ptr<zmq::socket_t> signal_socket_;
    std::function<void(const MarketDataSnapshot&)> data_handler_;
};

} // namespace api
} // namespace hft
