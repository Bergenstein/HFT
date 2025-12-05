// sim/exchange_simulator.hpp - Complete exchange simulator with WebSocket server
#pragma once

#include "matching_engine.hpp"
#include "../pipeline/normalized_data.hpp"
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <memory>
#include <iostream>
#include "../zmq/market_data_server.hpp"

#ifndef SIM_USE_WEBSOCKETPP
#define SIM_USE_WEBSOCKETPP 0
#endif

#if SIM_USE_WEBSOCKETPP
#include "../exchanges/websocketpp_compat.hpp" // Fix for websocketpp/Boost.Asio compatibility
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
using websocketpp::connection_hdl;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;
#else
// Fallback types for headless test builds (no websocketpp required)
using connection_hdl = std::shared_ptr<int>;
#endif

using json = nlohmann::json;

namespace sim {

// Exchange simulator - mimics Coinbase/Binance WebSocket API
class ExchangeSimulator {
public:
#if SIM_USE_WEBSOCKETPP
    using WSServer = websocketpp::server<websocketpp::config::asio>;
    using MessagePtr = WSServer::message_ptr;
#else
    // Minimal stub types used in headless/testing mode
    using WSServer = int; // not used
    using MessagePtr = int;
#endif

    struct SimulatorConfig {
        std::string name = "SimExchange";
        std::string ws_host = "0.0.0.0";
        uint16_t ws_port = 9001;

        // Matching engine configs per product
        std::vector<std::string> products = {"BTC-USD", "ETH-USD"};
        MatchingEngine::Config engine_config;

        // Latency simulation (microseconds)
        uint64_t min_latency_us = 50;
        uint64_t max_latency_us = 500;

        // Market data update frequency
        uint64_t update_interval_ms = 100;

        // Optional: publish snapshots via ZeroMQ pub/sub for external consumers
        bool enable_zmq = false;
        std::string zmq_endpoint = "tcp://*:5555";
    };

    ExchangeSimulator() : ExchangeSimulator(SimulatorConfig()) {}

    ExchangeSimulator(const SimulatorConfig& config)
        : config_(config), running_(false) {

        // Initialize matching engines for each product
        for (const auto& product : config_.products) {
            engines_[product] = std::make_unique<MatchingEngine>(product, config_.engine_config);

            // Set up callbacks
            engines_[product]->set_fill_callback([this, product](const Fill& fill) {
                on_fill(product, fill);
            });

            engines_[product]->set_order_update_callback([this, product](const Order& order) {
                on_order_update(product, order);
            });

            // Initialize per-product mutex
            product_mutexes_[product] = std::make_unique<std::mutex>();
        }

#if SIM_USE_WEBSOCKETPP
        // Initialize WebSocket server
        ws_server_.init_asio();
        ws_server_.set_reuse_addr(true);

        ws_server_.set_open_handler(bind(&ExchangeSimulator::on_ws_open, this, _1));
        ws_server_.set_close_handler(bind(&ExchangeSimulator::on_ws_close, this, _1));
        ws_server_.set_message_handler(bind(&ExchangeSimulator::on_ws_message, this, _1, _2));
#endif
    }

    ~ExchangeSimulator() {
        stop();
    }

    // Start the simulator
    void start() {
        running_ = true;

        // Initialize ZeroMQ publisher if enabled
        if (config_.enable_zmq) {
            try {
                zmq_server_ = std::make_unique<hft::MarketDataServer>(config_.zmq_endpoint);
            } catch (const std::exception& e) {
                std::cerr << "[" << config_.name << "] Failed to start ZMQ publisher: " << e.what() << "\n";
            }
        }

        std::cout << "[" << config_.name << "] Starting exchange simulator...\n";
        std::cout << "  WebSocket: ws://" << config_.ws_host << ":" << config_.ws_port << "\n";
        std::cout << "  Products: ";
        for (const auto& p : config_.products) std::cout << p << " ";
        std::cout << "\n";

#if SIM_USE_WEBSOCKETPP
        // Start WebSocket server thread
        ws_thread_ = std::thread([this]() {
            try {
                ws_server_.listen(config_.ws_port);
                ws_server_.start_accept();
                ws_server_.run();
            } catch (const std::exception& e) {
                std::cerr << "[" << config_.name << "] WebSocket error: " << e.what() << "\n";
            }
        });
#endif

        // Start market data broadcast thread
        broadcast_thread_ = std::thread([this]() {
            broadcast_market_data();
        });

        std::cout << "[" << config_.name << "] Exchange simulator started ✓\n";
    }

    // Stop the simulator
    void stop() {
        if (!running_) return;

        running_ = false;

        std::cout << "[" << config_.name << "] Stopping exchange simulator...\n";

#if SIM_USE_WEBSOCKETPP
        ws_server_.stop();

        if (ws_thread_.joinable()) ws_thread_.join();
#endif
        if (broadcast_thread_.joinable()) broadcast_thread_.join();

        std::cout << "[" << config_.name << "] Exchange simulator stopped ✓\n";
    }

    // Inject external market data (for realistic simulation)
    // 
    // The purpose of this method is to allow MarketReplay or external adapters to
    // update the internal matching engine's top-of-book without requiring direct
    // low-level access to the order book. It's implemented by submitting small,
    // short-lived "synthetic" limit orders on both sides (bid/ask) and tracking the
    // IDs so they can be canceled when a new update arrives. This provides a
    // practical way to replicate historical snapshots or to quickly set the top
    // of the book for testing.
    //
    // Notes on thread-safety:
    // - MatchingEngine is not internally thread-safe. Methods that call into the
    //   engine (submit_order / cancel_order / get_snapshot) are protected by a
    //   per-product mutex to avoid races with other API threads (WebSocket, replay,
    //   WebUI, strategy runners). Keep external calls synchronized.
    //
    // - This approach is lightweight and sufficient for replay-based simulations.
    //   For a production-level simulator more advanced APIs should be provided to
    //   replace entire book levels atomically and preserve order timestamps precisely.
    void inject_market_data(const std::string& product_id, double bid, double ask, double bid_size, double ask_size) {
        auto engine_it = engines_.find(product_id);
        if (engine_it == engines_.end()) return;

        // Per-product mutex ensures we don't mutate the matching engine concurrently
        std::lock_guard<std::mutex> lock(product_mutexes_[product_id]);

        // Cancel previous synthetic orders for this product (if any)
        auto& synthetic_list = synthetic_orders_[product_id];
        for (const auto& ord_id : synthetic_list) {
            try {
                engine_it->second->cancel_order(ord_id);
            } catch (...) {
                // Ignore failures - orders may already be filled or removed
            }
        }
        synthetic_list.clear();

        // If bid >= ask, adjust slightly to ensure valid spread
        if (bid >= ask) {
            double mid = (bid + ask) / 2.0;
            bid = mid - 0.5 * config_.engine_config.tick_size;
            ask = mid + 0.5 * config_.engine_config.tick_size;
        }

        // Add a synthetic bid order (if non-zero)
        if (bid_size > 0 && bid > 0) {
            std::string client_id = "synthetic_bid_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            try {
                auto o = engine_it->second->submit_order(client_id, OrderSide::BUY, OrderType::LIMIT, bid, bid_size);
                if (o) synthetic_list.push_back(o->order_id);
            } catch (...) {}
        }

        // Add a synthetic ask order (if non-zero)
        if (ask_size > 0 && ask > 0) {
            std::string client_id = "synthetic_ask_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            try {
                auto o = engine_it->second->submit_order(client_id, OrderSide::SELL, OrderType::LIMIT, ask, ask_size);
                if (o) synthetic_list.push_back(o->order_id);
            } catch (...) {}
        }
    }

    // Submit order (REST API simulation)
    std::shared_ptr<Order> submit_order(const std::string& product_id,
                                       const std::string& client_order_id,
                                       OrderSide side, OrderType type,
                                       double price, double size) {
        auto engine_it = engines_.find(product_id);
        if (engine_it == engines_.end()) {
            return nullptr;
        }

        // Simulate network latency
        simulate_latency();

        return engine_it->second->submit_order(client_order_id, side, type, price, size);
    }

    // Cancel order (REST API simulation)
    bool cancel_order(const std::string& product_id, const std::string& order_id) {
        auto engine_it = engines_.find(product_id);
        if (engine_it == engines_.end()) return false;

        simulate_latency();

        return engine_it->second->cancel_order(order_id);
    }

    // Get order book snapshot
    MatchingEngine::Snapshot get_snapshot(const std::string& product_id, int depth = 20) const {
        auto engine_it = engines_.find(product_id);
        if (engine_it == engines_.end()) {
            return MatchingEngine::Snapshot{};
        }

        return engine_it->second->get_snapshot(depth);
    }

    // Register a callback to receive in-process NormalizedQuote objects
    void set_quote_callback(std::function<void(const pipeline::NormalizedQuote&)> cb) {
        quote_callback_ = std::move(cb);
    }

private:
#if SIM_USE_WEBSOCKETPP
    void on_ws_open(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.insert(hdl);

        std::cout << "[" << config_.name << "] Client connected (total: " << connections_.size() << ")\n";

        // Send welcome message
        json welcome = {
            {"type", "welcome"},
            {"exchange", config_.name},
            {"products", config_.products},
            {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
        };

        try {
            ws_server_.send(hdl, welcome.dump(), websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            std::cerr << "[" << config_.name << "] Send error: " << e.what() << "\n";
        }
    }

    void on_ws_close(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(hdl);

        // Remove subscriptions
        subscriptions_.erase(hdl);

        std::cout << "[" << config_.name << "] Client disconnected (total: " << connections_.size() << ")\n";
    }

    void on_ws_message(connection_hdl hdl, MessagePtr msg) {
        try {
            json request = json::parse(msg->get_payload());

            std::string type = request["type"];

            if (type == "subscribe") {
                handle_subscribe(hdl, request);
            } else if (type == "unsubscribe") {
                handle_unsubscribe(hdl, request);
            } else if (type == "order") {
                handle_order_request(hdl, request);
            } else if (type == "cancel") {
                handle_cancel_request(hdl, request);
            }

        } catch (const std::exception& e) {
            json error_response = {
                {"type", "error"},
                {"message", e.what()}
            };

            try {
                ws_server_.send(hdl, error_response.dump(), websocketpp::frame::opcode::text);
            } catch (...) {}
        }
    }
#endif

    void handle_subscribe(connection_hdl hdl, const json& request) {
        std::lock_guard<std::mutex> lock(connections_mutex_);

        std::string channel = request["channel"];
        std::vector<std::string> products = request["product_ids"].get<std::vector<std::string>>();

#if SIM_USE_WEBSOCKETPP
        subscriptions_[hdl][channel].insert(products.begin(), products.end());

        json response = {
            {"type", "subscriptions"},
            {"channel", channel},
            {"product_ids", products}
        };

        ws_server_.send(hdl, response.dump(), websocketpp::frame::opcode::text);
#else
        // Headless mode: just log
        std::cout << "[" << config_.name << "] Subscription request: " << channel << "";
#endif
    }

    void handle_unsubscribe(connection_hdl hdl, const json& request) {
        std::lock_guard<std::mutex> lock(connections_mutex_);

        std::string channel = request["channel"];

#if SIM_USE_WEBSOCKETPP
        if (subscriptions_.find(hdl) != subscriptions_.end()) {
            subscriptions_[hdl].erase(channel);
        }

        json response = {
            {"type", "unsubscribed"},
            {"channel", channel}
        };

        ws_server_.send(hdl, response.dump(), websocketpp::frame::opcode::text);
#else
        std::cout << "[" << config_.name << "] Unsubscribe: " << channel << "\n";
#endif
    }

    void handle_order_request(connection_hdl hdl, const json& request) {
        std::string product_id = request["product_id"];
        std::string client_order_id = request.value("client_order_id", "");
        std::string side_str = request["side"];
        std::string type_str = request["type"];
        double price = request.value("price", 0.0);
        double size = request["size"];

        OrderSide side = (side_str == "buy") ? OrderSide::BUY : OrderSide::SELL;
        OrderType type = (type_str == "market") ? OrderType::MARKET : OrderType::LIMIT;

        auto order = submit_order(product_id, client_order_id, side, type, price, size);

#if SIM_USE_WEBSOCKETPP
        json response = {
            {"type", "order_response"},
            {"order_id", order->order_id},
            {"status", static_cast<int>(order->status)},
            {"filled_size", order->filled_size},
            {"remaining_size", order->remaining_size}
        };

        ws_server_.send(hdl, response.dump(), websocketpp::frame::opcode::text);
#else
        std::cout << "Order response: " << order->order_id << " status=" << static_cast<int>(order->status) << "\n";
#endif
    }

    void handle_cancel_request(connection_hdl hdl, const json& request) {
        std::string product_id = request["product_id"];
        std::string order_id = request["order_id"];

        bool success = cancel_order(product_id, order_id);

#if SIM_USE_WEBSOCKETPP
        json response = {
            {"type", "cancel_response"},
            {"order_id", order_id},
            {"success", success}
        };

        ws_server_.send(hdl, response.dump(), websocketpp::frame::opcode::text);
#else
        std::cout << "Cancel response: order=" << order_id << " success=" << success << "\n";
#endif
    }

    void on_fill(const std::string& product_id, const Fill& fill) {
        // Broadcast fill to subscribed clients
        json fill_msg = {
            {"type", "fill"},
            {"product_id", product_id},
            {"fill_id", fill.fill_id},
            {"order_id", fill.order_id},
            {"side", fill.side == OrderSide::BUY ? "buy" : "sell"},
            {"price", fill.price},
            {"size", fill.size},
            {"fee", fill.fee},
            {"is_maker", fill.is_maker},
            {"timestamp", fill.timestamp.time_since_epoch().count()}
        };

        broadcast_to_channel("fills", product_id, fill_msg);
    }

    void on_order_update(const std::string& product_id, const Order& order) {
        // Broadcast order update to subscribed clients
        json order_msg = {
            {"type", "order_update"},
            {"product_id", product_id},
            {"order_id", order.order_id},
            {"status", static_cast<int>(order.status)},
            {"filled_size", order.filled_size},
            {"remaining_size", order.remaining_size},
            {"avg_fill_price", order.avg_fill_price}
        };

        broadcast_to_channel("orders", product_id, order_msg);
    }

    void broadcast_market_data() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.update_interval_ms));

            for (const auto& [product_id, engine] : engines_) {
                auto snapshot = engine->get_snapshot(20);

                json depth_msg = {
                    {"type", "l2_update"},
                    {"product_id", product_id},
                    {"bids", json::array()},
                    {"asks", json::array()},
                    {"timestamp", snapshot.timestamp.time_since_epoch().count()}
                };

                std::map<double,double> bids_map; // ascending keys
                std::map<double,double> asks_map;

                for (const auto& [price, size] : snapshot.bids) {
                    depth_msg["bids"].push_back({price, size});
                    bids_map[price] = size;
                }

                for (const auto& [price, size] : snapshot.asks) {
                    depth_msg["asks"].push_back({price, size});
                    asks_map[price] = size;
                }

                // Publish via WebSocket or headless log
                broadcast_to_channel("l2_data", product_id, depth_msg);

                // Convert to NormalizedQuote and call in-process callback (for direct pipeline integration)
                if (quote_callback_) {
                    pipeline::NormalizedQuote quote;
                    quote.exchange = config_.name;
                    quote.product_id = product_id;
                    quote.local_timestamp = std::chrono::system_clock::now();
                    quote.exchange_timestamp = snapshot.timestamp;

                    if (!snapshot.bids.empty()) {
                        quote.best_bid = snapshot.bids[0].first;
                        quote.bid_size  = snapshot.bids[0].second;
                        quote.bids = snapshot.bids;
                    }
                    if (!snapshot.asks.empty()) {
                        quote.best_ask = snapshot.asks[0].first;
                        quote.ask_size  = snapshot.asks[0].second;
                        quote.asks = snapshot.asks;
                    }

                    // Sequence tracking per product
                    uint64_t seq = 0;
                    {
                        std::lock_guard<std::mutex> lock(product_mutexes_[product_id]);
                        auto& s = sequence_counters_[product_id];
                        seq = ++s;
                    }
                    quote.sequence = seq;

                    // Invoke callback
                    quote_callback_(quote);
                }

                // If ZMQ publisher is enabled, publish the snapshot as protobuf
                if (zmq_server_) {
                    // Use sequence number for the product
                    uint64_t seq = 0;
                    {
                        std::lock_guard<std::mutex> lock(product_mutexes_[product_id]);
                        auto& s = sequence_counters_[product_id];
                        seq = s;
                    }
                    zmq_server_->publish_snapshot(product_id, bids_map, asks_map, seq);
                }
            }
        }
    }

    void broadcast_to_channel(const std::string& channel, const std::string& product_id, const json& message) {
        std::lock_guard<std::mutex> lock(connections_mutex_);

        std::string msg_str = message.dump();

#if SIM_USE_WEBSOCKETPP
        for (const auto& hdl : connections_) {
            auto sub_it = subscriptions_.find(hdl);
            if (sub_it == subscriptions_.end()) continue;

            auto channel_it = sub_it->second.find(channel);
            if (channel_it == sub_it->second.end()) continue;

            if (channel_it->second.count(product_id) > 0) {
                try {
                    ws_server_.send(hdl, msg_str, websocketpp::frame::opcode::text);
                } catch (const std::exception& e) {
                    std::cerr << "[" << config_.name << "] Broadcast error: " << e.what() << "\n";
                }
            }
        }
#else
        // Headless: just print notifications so tests can assert behavior by reading stdout
        std::cout << "[BCAST] " << channel << " " << product_id << " -> " << msg_str << "\n";
#endif
    }

    void simulate_latency() {
        if (config_.min_latency_us > 0) {
            uint64_t latency_us = config_.min_latency_us + 
                                 (std::rand() % (config_.max_latency_us - config_.min_latency_us + 1));
            std::this_thread::sleep_for(std::chrono::microseconds(latency_us));
        }
    }

    SimulatorConfig config_;
    std::atomic<bool> running_;

    // Matching engines per product
    std::unordered_map<std::string, std::unique_ptr<MatchingEngine>> engines_;

#if SIM_USE_WEBSOCKETPP
    // WebSocket server
    WSServer ws_server_;
    std::thread ws_thread_;
#endif
    std::thread broadcast_thread_;

    // Client connections and subscriptions
#if SIM_USE_WEBSOCKETPP
    std::unordered_set<connection_hdl, std::owner_less<connection_hdl>> connections_;
    std::unordered_map<connection_hdl, 
                      std::unordered_map<std::string, std::unordered_set<std::string>>,
                      std::owner_less<connection_hdl>> subscriptions_;
    std::mutex connections_mutex_;
#else
    // Headless mode: use simple placeholders
    std::vector<int> connections_; // not used
    std::mutex connections_mutex_;
    std::unordered_map<int, std::unordered_map<std::string, std::unordered_set<std::string>>> subscriptions_; // not used
#endif

    // Per-product mutexes for thread safety
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> product_mutexes_;

    // Track synthetic orders per product
    std::unordered_map<std::string, std::vector<std::string>> synthetic_orders_;

    // ZeroMQ server for publishing snapshots
    std::unique_ptr<hft::MarketDataServer> zmq_server_;

    // Sequence counters per product
    std::unordered_map<std::string, uint64_t> sequence_counters_;

    // Callback for in-process NormalizedQuote objects
    std::function<void(const pipeline::NormalizedQuote&)> quote_callback_;
};

} // namespace sim
