// tests/test_coinbase_l2.cpp - Test Coinbase L2 Order Book Data
#include "../exchanges/coinbase_ws_client.hpp"
#include "../exchanges/coinbase_normalizer.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <iostream>
#include <thread>
#include <chrono>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;

int main() {
    std::cout << "================================================================\n";
    std::cout << "  Coinbase L2 Order Book Test\n";
    std::cout << "================================================================\n\n";
    
    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        
        // Load certificates for Coinbase SSL
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);
        
        exchanges::CoinbaseWebSocketClient client(ioc, ctx);
        
        int msg_count = 0;
        int snapshot_count = 0;
        int update_count = 0;
        
        auto callback = [&](const json& msg) {
            msg_count++;
            
            if (msg.contains("type")) {
                std::string type = msg["type"].get<std::string>();
                std::cout << "[" << msg_count << "] Type: " << type;
                
                if (msg.contains("product_id")) {
                    std::cout << " | Product: " << msg["product_id"].get<std::string>();
                }
                std::cout << "\n";
                
                // Normalize the update
                auto update = exchanges::CoinbaseNormalizer::normalize_l2_update(msg);
                
                if (!update.symbol.empty()) {
                    if (update.is_snapshot) {
                        snapshot_count++;
                        std::cout << "  └─ SNAPSHOT: " << update.symbol 
                                  << " | Bids: " << update.bids.size() 
                                  << " | Asks: " << update.asks.size() << "\n";
                        
                        if (!update.bids.empty() && !update.asks.empty()) {
                            std::cout << "     Best Bid: " << update.bids[0].price << " @ " << update.bids[0].quantity << "\n";
                            std::cout << "     Best Ask: " << update.asks[0].price << " @ " << update.asks[0].quantity << "\n";
                            std::cout << "     Spread: " << (update.asks[0].price - update.bids[0].price) << "\n";
                        }
                    } else {
                        update_count++;
                        if (update_count <= 10) {  // Only print first 10 updates
                            std::cout << "  └─ UPDATE: " << update.symbol 
                                      << " | Changes: " << (update.bids.size() + update.asks.size()) << "\n";
                        }
                    }
                }
            }
            
            // Stop after receiving 1 snapshot and 100+ updates
            if (snapshot_count >= 1 && update_count >= 100) {
                std::cout << "\n[Test] Received " << msg_count << " messages\n";
                std::cout << "  └─ Snapshots: " << snapshot_count << "\n";
                std::cout << "  └─ Updates: " << update_count << "\n";
                std::cout << "[Test] ✅ Coinbase L2 test PASSED\n";
                ioc.stop();
            }
        };
        
        // Test with BTC-USD and ETH-USD
        std::vector<std::string> products = {"BTC-USD", "ETH-USD"};
        
        std::cout << "Connecting to Coinbase...\n";
        client.connect(products, callback);
        
        // Run for 60 seconds max
        std::thread timeout_thread([&ioc]() {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            std::cout << "\n[Test] Timeout reached\n";
            ioc.stop();
        });
        
        ioc.run();
        
        timeout_thread.join();
        
        if (msg_count == 0) {
            std::cerr << "[Test] ❌ FAILED - No messages received\n";
            return 1;
        }
        
        if (snapshot_count == 0) {
            std::cerr << "[Test] ⚠️  WARNING - No snapshots received\n";
        }
        
        std::cout << "\n[Test] Connection closed successfully\n";
        
    } catch (const std::exception& e) {
        std::cerr << "[Test] Exception: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
