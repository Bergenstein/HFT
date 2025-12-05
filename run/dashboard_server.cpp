//==============================================================================
// DASHBOARD SERVER
//==============================================================================
// Bridges ZMQ metrics publisher to WebSocket for browser dashboard consumption.
// This runs on the cold path (non-latency critical).
//
// Architecture:
//   [Strategy Engine] -> [ZMQ Publisher] -> [This Server] -> [WebSocket] -> [Browser Dashboard]
//
// Usage:
//   ./build/dashboard_server [zmq_endpoint] [websocket_port]
//
//==============================================================================

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>
#include <fstream>
#include <sstream>
#include <random>
#include <iomanip>

#include "../zmq/strategy_metrics_publisher.hpp"

// For this example, we'll create a simple HTTP server that serves the dashboard
// and publishes updates. In production, you'd use a proper WebSocket library.

std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\n[SIGNAL] Caught signal " << sig << ", shutting down...\n";
    g_running.store(false);
}

// Simple metrics generator for testing
void generate_sample_metrics(zmq_metrics::StrategyMetricsPublisher& publisher) {
    double equity = 100000.0;
    double peak_equity = equity;
    int trades = 0;
    int winners = 0;
    double total_pnl = 0.0;
    double funding_collected = 0.0;
    
    std::mt19937 rng(42);
    std::normal_distribution<double> returns(0.0002, 0.003); // Slight positive bias
    
    std::cout << "[GENERATOR] Starting metrics generator...\n";
    
    while (g_running.load()) {
        // Simulate P&L
        double daily_return = returns(rng);
        double pnl_change = equity * daily_return;
        equity += pnl_change;
        total_pnl += pnl_change;
        
        if (pnl_change > 0) {
            winners++;
            funding_collected += pnl_change * 0.8; // 80% from funding
        }
        trades++;
        
        peak_equity = std::max(peak_equity, equity);
        double drawdown = (peak_equity - equity) / peak_equity;
        
        // Calculate metrics
        zmq_metrics::StrategyMetrics m;
        m.strategy_name = "FundingRateArb";
        m.symbol = "MULTI";
        m.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        m.total_pnl = total_pnl;
        m.unrealized_pnl = 0;
        m.realized_pnl = total_pnl;
        m.daily_pnl = pnl_change;
        
        m.position_size = 50000;
        m.position_value = 50000;
        m.avg_entry_price = 97000;
        
        m.total_trades = trades;
        m.winning_trades = winners;
        m.losing_trades = trades - winners;
        m.win_rate = static_cast<double>(winners) / trades;
        m.avg_trade_pnl = total_pnl / trades;
        
        // Daily Sharpe (NOT annualized)
        m.sharpe_ratio = 0.15 + (rng() % 100) / 1000.0; // ~0.15-0.25
        m.sortino_ratio = m.sharpe_ratio * 1.3;
        m.max_drawdown = std::max(drawdown, 0.02);
        m.current_drawdown = drawdown;
        m.volatility = 0.015 + (rng() % 50) / 10000.0;
        m.calmar_ratio = (total_pnl / 100000.0) / std::max(m.max_drawdown, 0.01);
        
        m.funding_collected = funding_collected;
        m.avg_funding_diff_bps = 7.5 + (rng() % 100) / 20.0;
        m.hours_in_position = trades * 8.0;
        m.funding_periods = trades;
        
        m.latency_us = 50 + rng() % 100;
        m.signals_generated = trades * 3;
        m.orders_sent = trades * 2;
        m.fills_received = trades * 2;
        
        publisher.publish_metrics(m);
        
        // Equity point
        zmq_metrics::EquityCurvePoint ep;
        ep.timestamp_ms = m.timestamp_ms;
        ep.equity = equity;
        ep.drawdown = drawdown;
        publisher.publish_equity(ep);
        
        // Every 10 updates, print status
        if (trades % 10 == 0) {
            std::cout << "[METRICS] P&L: $" << std::fixed << std::setprecision(2) << total_pnl
                      << " | Equity: $" << equity
                      << " | Sharpe: " << std::setprecision(3) << m.sharpe_ratio
                      << " | DD: " << std::setprecision(2) << (drawdown * 100) << "%\n";
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --zmq-endpoint <addr>   ZMQ publish endpoint (default: tcp://*:5555)\n";
    std::cout << "  --generate              Generate sample metrics (for testing)\n";
    std::cout << "  --help                  Show this help\n";
    std::cout << "\nThe dashboard HTML file is at: dashboard/strategy_dashboard.html\n";
    std::cout << "Open it in a browser to view the dashboard.\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::string zmq_endpoint = "tcp://*:5555";
    bool generate = false;
    
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--zmq-endpoint") == 0 && i + 1 < argc) {
            zmq_endpoint = argv[++i];
        } else if (strcmp(argv[i], "--generate") == 0) {
            generate = true;
        }
    }
    
    std::cout << R"(
╔══════════════════════════════════════════════════════════════════════════════╗
║                       STRATEGY METRICS DASHBOARD SERVER                       ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  Cold Path Service - Publishes strategy metrics via ZMQ                       ║
║  Dashboard: Open dashboard/strategy_dashboard.html in browser                 ║
╚══════════════════════════════════════════════════════════════════════════════╝
)" << "\n";
    
    // Create publisher
    zmq_metrics::StrategyMetricsPublisher publisher(zmq_endpoint);
    publisher.start();
    
    std::cout << "[SERVER] ZMQ publisher started on " << zmq_endpoint << "\n";
    std::cout << "[SERVER] Press Ctrl+C to stop\n\n";
    
    if (generate) {
        std::cout << "[SERVER] Generating sample metrics...\n";
        generate_sample_metrics(publisher);
    } else {
        std::cout << "[SERVER] Waiting for metrics from strategy engines...\n";
        std::cout << "[SERVER] (Use --generate flag for demo mode)\n";
        
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    publisher.stop();
    std::cout << "[SERVER] Stopped.\n";
    
    return 0;
}
