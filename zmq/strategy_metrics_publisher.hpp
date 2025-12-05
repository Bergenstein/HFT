#pragma once

//==============================================================================
// STRATEGY METRICS ZMQ PUBLISHER
//==============================================================================
// Publishes strategy performance metrics via ZMQ for cold path dashboard
// consumption. Designed to be non-blocking and not interfere with hot path.
//
// Architecture:
//   [Strategy Engine] -> [Metrics Queue] -> [ZMQ Publisher] -> [Dashboard]
//
// Subscribers (dashboards) can connect and receive real-time metrics without
// affecting trading latency.
//==============================================================================

#include <zmq.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>

namespace zmq_metrics {

using json = nlohmann::json;

//==============================================================================
// METRICS DATA STRUCTURES
//==============================================================================

struct StrategyMetrics {
    // Identity
    std::string strategy_name;
    std::string symbol;
    int64_t timestamp_ms;
    
    // P&L (not annualized)
    double total_pnl;
    double unrealized_pnl;
    double realized_pnl;
    double daily_pnl;
    
    // Position
    double position_size;
    double position_value;
    double avg_entry_price;
    
    // Trade Stats
    int total_trades;
    int winning_trades;
    int losing_trades;
    double win_rate;          // Not annualized
    double avg_trade_pnl;
    
    // Risk Metrics (NOT ANNUALIZED - as per user request)
    double sharpe_ratio;      // Daily Sharpe
    double sortino_ratio;     // Daily Sortino
    double max_drawdown;
    double current_drawdown;
    double volatility;        // Daily volatility
    double calmar_ratio;      // Not annualized
    
    // Funding Rate Specific (for funding arb strategy)
    double funding_collected;
    double avg_funding_diff_bps;
    double hours_in_position;
    int funding_periods;
    
    // Performance
    double latency_us;        // Last signal latency in microseconds
    int signals_generated;
    int orders_sent;
    int fills_received;
    
    json to_json() const {
        return {
            {"strategy_name", strategy_name},
            {"symbol", symbol},
            {"timestamp_ms", timestamp_ms},
            {"pnl", {
                {"total", total_pnl},
                {"unrealized", unrealized_pnl},
                {"realized", realized_pnl},
                {"daily", daily_pnl}
            }},
            {"position", {
                {"size", position_size},
                {"value", position_value},
                {"avg_entry", avg_entry_price}
            }},
            {"trades", {
                {"total", total_trades},
                {"winners", winning_trades},
                {"losers", losing_trades},
                {"win_rate", win_rate},
                {"avg_pnl", avg_trade_pnl}
            }},
            {"risk", {
                {"sharpe_daily", sharpe_ratio},
                {"sortino_daily", sortino_ratio},
                {"max_drawdown", max_drawdown},
                {"current_drawdown", current_drawdown},
                {"volatility_daily", volatility},
                {"calmar", calmar_ratio}
            }},
            {"funding", {
                {"collected", funding_collected},
                {"avg_diff_bps", avg_funding_diff_bps},
                {"hours_in_position", hours_in_position},
                {"periods", funding_periods}
            }},
            {"performance", {
                {"latency_us", latency_us},
                {"signals", signals_generated},
                {"orders", orders_sent},
                {"fills", fills_received}
            }}
        };
    }
    
    static StrategyMetrics from_json(const json& j) {
        StrategyMetrics m;
        m.strategy_name = j.at("strategy_name").get<std::string>();
        m.symbol = j.at("symbol").get<std::string>();
        m.timestamp_ms = j.at("timestamp_ms").get<int64_t>();
        
        const auto& pnl = j.at("pnl");
        m.total_pnl = pnl.at("total").get<double>();
        m.unrealized_pnl = pnl.at("unrealized").get<double>();
        m.realized_pnl = pnl.at("realized").get<double>();
        m.daily_pnl = pnl.at("daily").get<double>();
        
        const auto& pos = j.at("position");
        m.position_size = pos.at("size").get<double>();
        m.position_value = pos.at("value").get<double>();
        m.avg_entry_price = pos.at("avg_entry").get<double>();
        
        const auto& trades = j.at("trades");
        m.total_trades = trades.at("total").get<int>();
        m.winning_trades = trades.at("winners").get<int>();
        m.losing_trades = trades.at("losers").get<int>();
        m.win_rate = trades.at("win_rate").get<double>();
        m.avg_trade_pnl = trades.at("avg_pnl").get<double>();
        
        const auto& risk = j.at("risk");
        m.sharpe_ratio = risk.at("sharpe_daily").get<double>();
        m.sortino_ratio = risk.at("sortino_daily").get<double>();
        m.max_drawdown = risk.at("max_drawdown").get<double>();
        m.current_drawdown = risk.at("current_drawdown").get<double>();
        m.volatility = risk.at("volatility_daily").get<double>();
        m.calmar_ratio = risk.at("calmar").get<double>();
        
        const auto& funding = j.at("funding");
        m.funding_collected = funding.at("collected").get<double>();
        m.avg_funding_diff_bps = funding.at("avg_diff_bps").get<double>();
        m.hours_in_position = funding.at("hours_in_position").get<double>();
        m.funding_periods = funding.at("periods").get<int>();
        
        const auto& perf = j.at("performance");
        m.latency_us = perf.at("latency_us").get<double>();
        m.signals_generated = perf.at("signals").get<int>();
        m.orders_sent = perf.at("orders").get<int>();
        m.fills_received = perf.at("fills").get<int>();
        
        return m;
    }
};

struct EquityCurvePoint {
    int64_t timestamp_ms;
    double equity;
    double drawdown;
    
    json to_json() const {
        return {
            {"timestamp_ms", timestamp_ms},
            {"equity", equity},
            {"drawdown", drawdown}
        };
    }
};

//==============================================================================
// ZMQ METRICS PUBLISHER
//==============================================================================

class StrategyMetricsPublisher {
public:
    StrategyMetricsPublisher(const std::string& endpoint = "tcp://*:5555")
        : endpoint_(endpoint), running_(false) {}
    
    ~StrategyMetricsPublisher() {
        stop();
    }
    
    void start() {
        if (running_) return;
        running_ = true;
        thread_ = std::thread(&StrategyMetricsPublisher::run, this);
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    
    // Queue metrics for publishing (non-blocking, thread-safe)
    void publish_metrics(const StrategyMetrics& metrics) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (metrics_queue_.size() < 10000) {  // Prevent unbounded growth
            metrics_queue_.push(metrics);
        }
    }
    
    // Queue equity point for publishing
    void publish_equity(const EquityCurvePoint& point) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (equity_queue_.size() < 10000) {
            equity_queue_.push(point);
        }
    }
    
private:
    void run() {
        zmq::context_t context(1);
        zmq::socket_t publisher(context, zmq::socket_type::pub);
        
        try {
            publisher.bind(endpoint_);
            std::cout << "[ZMQ] Metrics publisher bound to " << endpoint_ << "\n";
        } catch (const zmq::error_t& e) {
            std::cerr << "[ZMQ] Failed to bind: " << e.what() << "\n";
            return;
        }
        
        while (running_) {
            // Process metrics queue
            {
                std::lock_guard<std::mutex> lock(mutex_);
                while (!metrics_queue_.empty()) {
                    const auto& m = metrics_queue_.front();
                    json j = m.to_json();
                    std::string topic = "METRICS." + m.strategy_name + "." + m.symbol;
                    std::string payload = j.dump();
                    
                    // Send topic
                    zmq::message_t topic_msg(topic.data(), topic.size());
                    publisher.send(topic_msg, zmq::send_flags::sndmore);
                    
                    // Send payload
                    zmq::message_t payload_msg(payload.data(), payload.size());
                    publisher.send(payload_msg, zmq::send_flags::none);
                    
                    metrics_queue_.pop();
                }
                
                while (!equity_queue_.empty()) {
                    const auto& e = equity_queue_.front();
                    json j = e.to_json();
                    std::string topic = "EQUITY";
                    std::string payload = j.dump();
                    
                    zmq::message_t topic_msg(topic.data(), topic.size());
                    publisher.send(topic_msg, zmq::send_flags::sndmore);
                    
                    zmq::message_t payload_msg(payload.data(), payload.size());
                    publisher.send(payload_msg, zmq::send_flags::none);
                    
                    equity_queue_.pop();
                }
            }
            
            // Small sleep to prevent busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::string endpoint_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::mutex mutex_;
    std::queue<StrategyMetrics> metrics_queue_;
    std::queue<EquityCurvePoint> equity_queue_;
};

//==============================================================================
// ZMQ METRICS SUBSCRIBER (for dashboard)
//==============================================================================

class StrategyMetricsSubscriber {
public:
    using MetricsCallback = std::function<void(const StrategyMetrics&)>;
    using EquityCallback = std::function<void(const EquityCurvePoint&)>;
    
    StrategyMetricsSubscriber(const std::string& endpoint = "tcp://localhost:5555")
        : endpoint_(endpoint), running_(false) {}
    
    ~StrategyMetricsSubscriber() {
        stop();
    }
    
    void set_metrics_callback(MetricsCallback cb) {
        metrics_callback_ = std::move(cb);
    }
    
    void set_equity_callback(EquityCallback cb) {
        equity_callback_ = std::move(cb);
    }
    
    void start() {
        if (running_) return;
        running_ = true;
        thread_ = std::thread(&StrategyMetricsSubscriber::run, this);
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    
private:
    void run() {
        zmq::context_t context(1);
        zmq::socket_t subscriber(context, zmq::socket_type::sub);
        
        try {
            subscriber.connect(endpoint_);
            subscriber.set(zmq::sockopt::subscribe, "");  // Subscribe to all
            std::cout << "[ZMQ] Subscriber connected to " << endpoint_ << "\n";
        } catch (const zmq::error_t& e) {
            std::cerr << "[ZMQ] Failed to connect: " << e.what() << "\n";
            return;
        }
        
        while (running_) {
            zmq::message_t topic_msg, payload_msg;
            
            zmq::recv_result_t result = subscriber.recv(topic_msg, zmq::recv_flags::dontwait);
            if (result) {
                subscriber.recv(payload_msg, zmq::recv_flags::none);
                
                std::string topic(static_cast<char*>(topic_msg.data()), topic_msg.size());
                std::string payload(static_cast<char*>(payload_msg.data()), payload_msg.size());
                
                try {
                    json j = json::parse(payload);
                    
                    if (topic.find("METRICS.") == 0 && metrics_callback_) {
                        StrategyMetrics m = StrategyMetrics::from_json(j);
                        metrics_callback_(m);
                    } else if (topic == "EQUITY" && equity_callback_) {
                        EquityCurvePoint e;
                        e.timestamp_ms = j.at("timestamp_ms").get<int64_t>();
                        e.equity = j.at("equity").get<double>();
                        e.drawdown = j.at("drawdown").get<double>();
                        equity_callback_(e);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[ZMQ] Parse error: " << e.what() << "\n";
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    
    std::string endpoint_;
    std::atomic<bool> running_;
    std::thread thread_;
    MetricsCallback metrics_callback_;
    EquityCallback equity_callback_;
};

} // namespace zmq_metrics
