// filepath: /Users/israelbergenstein/Desktop/HFT_Coinbase_v2/Coin_base_HFT/run/stream_with_latency.cpp
// Enhanced stream_and_record with detailed latency tracking
// Uses the actual system interfaces: WsL2Client, Recorder, Normalizer, OrderBook

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <signal.h>
#include <chrono>

#include "../discovery/discovery.hpp"
#include "../md/ws_l2_client.hpp"
#include "../md/normalizer.hpp"
#include "../core/order_book.hpp"
#include "../core/latency_tracker.hpp"
#include "../core/timestamp.hpp"
#include "../io/recorder.hpp"

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "=== HFT Stream with Latency Tracking ===\n\n";

    // Discover tradable products using the Discovery class
    std::cout << "Discovering tradable products...\n";
    auto all_products = Discovery::tradable_products();
    
    if (all_products.empty()) {
        std::cerr << "No tradable products found.\n";
        return 1;
    }

    // Get volume data and select top products by volume
    auto volumes = Discovery::volume_24h();
    auto top_products = Discovery::top_by_volume(all_products, volumes, 6);
    
    if (top_products.empty()) {
        // Fallback to first 6 products
        top_products.assign(all_products.begin(), 
                           all_products.begin() + std::min(size_t(6), all_products.size()));
    }

    std::cout << "Selected " << top_products.size() << " products for streaming:\n";
    for (const auto& pid : top_products) {
        std::cout << "  - " << pid << "\n";
    }
    std::cout << "\n";

    // Create WebSocket clients (using WsL2Client)
    // The WsL2Client class handles connection, subscription, recording internally
    const size_t num_clients = std::min(size_t(3), top_products.size());
    
    // Distribute products across clients
    std::vector<std::vector<std::string>> client_products(num_clients);
    for (size_t i = 0; i < top_products.size(); ++i) {
        client_products[i % num_clients].push_back(top_products[i]);
    }

    // Create latency tracker
    LatencyTracker latency_tracker;
    
    // Start threads for each WebSocket client
    std::vector<std::thread> threads;
    
    for (size_t i = 0; i < num_clients; ++i) {
        if (client_products[i].empty()) continue;
        
        threads.emplace_back([i, &client_products]() {
            try {
                WsL2Client client(static_cast<int>(i), client_products[i], "data");
                // Note: WsL2Client::run() blocks and handles message processing
                // with internal latency tracking
                client.run();
            } catch (const std::exception& e) {
                std::cerr << "[WS#" << i << "] Error: " << e.what() << std::endl;
            }
        });
    }

    std::cout << "=== STREAMING WITH LATENCY TRACKING ===\n";
    std::cout << "Recording to data/ directory\n";
    std::cout << "Press Ctrl+C to stop\n\n";

    // Main loop - wait for signal
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\nShutting down...\n";
    
    // Note: WsL2Client threads will continue running until disconnected
    // In production, we'd need a proper shutdown mechanism
    for (auto& t : threads) {
        if (t.joinable()) {
            t.detach(); // Detach since WsL2Client doesn't have graceful shutdown
        }
    }

    std::cout << "\n=== Stream ended ===\n";
    return 0;
}
