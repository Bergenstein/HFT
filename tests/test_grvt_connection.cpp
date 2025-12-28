// tests/test_grvt_connection.cpp - Test GRVT WebSocket Connection and L2 Data
#include "../exchanges/grvt_ws_client.hpp"
#include "../exchanges/grvt_normalizer.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <iostream>
#include <thread>
#include <chrono>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;

int main() {
    std::cout << "================================================================\n";
    std::cout << "  GRVT WebSocket Connection Test\n";
    std::cout << "================================================================\n\n";
    
    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        
        // Don't verify SSL for testing (use proper certs in production)
        ctx.set_verify_mode(ssl::verify_none);
        
        exchanges::GRVTWebSocketClient client(ioc, ctx);
        
        int msg_count = 0;
        int orderbook_updates = 0;
        
        auto callback = [&](const json& msg) {
            msg_count++;
            
            std::cout << "[" << msg_count << "] Received: " << msg.dump().substr(0, 200) << "...\n";
            
            // Try to normalize the message
            auto update = exchanges::GRVTNormalizer::normalize_l2_update(msg);
            if (!update.symbol.empty()) {
                orderbook_updates++;
                std::cout << "  └─ Normalized L2 Update: " << update.symbol 
                          << " | Bids: " << update.bids.size() 
                          << " | Asks: " << update.asks.size()
                          << " | Snapshot: " << (update.is_snapshot ? "YES" : "NO")
                          << "\n";
                
                if (!update.bids.empty()) {
                    std::cout << "     Best Bid: " << update.bids[0].price << " @ " << update.bids[0].quantity << "\n";
                }
                if (!update.asks.empty()) {
                    std::cout << "     Best Ask: " << update.asks[0].price << " @ " << update.asks[0].quantity << "\n";
                }
            }
            
            // Stop after 50 messages
            if (msg_count >= 50) {
                std::cout << "\n[Test] Received " << msg_count << " messages, " 
                          << orderbook_updates << " orderbook updates\n";
                std::cout << "[Test] ✅ GRVT connection test PASSED\n";
                ioc.stop();
            }
        };
        
        // Test with BTC and ETH
        std::vector<std::string> symbols = {"BTC-USDT", "ETH-USDT"};
        
        std::cout << "Connecting to GRVT...\n";
        client.connect(symbols, callback);
        
        // Run for 30 seconds max
        std::thread timeout_thread([&ioc]() {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            std::cout << "\n[Test] Timeout reached\n";
            ioc.stop();
        });
        
        ioc.run();
        
        timeout_thread.join();
        
        if (msg_count == 0) {
            std::cerr << "[Test] ❌ FAILED - No messages received\n";
            return 1;
        }
        
        if (orderbook_updates == 0) {
            std::cerr << "[Test] ⚠️  WARNING - No orderbook updates parsed\n";
        }
        
        std::cout << "\n[Test] Connection closed successfully\n";
        
    } catch (const std::exception& e) {
        std::cerr << "[Test] Exception: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
