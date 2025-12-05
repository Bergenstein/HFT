// filepath: /Users/israelbergenstein/Desktop/Desktop - Israel's MacBook Pro/MacBookWork/All_Desktop /Work/HFT/HFT_Coinbase/Coin_base_HFT/run/test_zmq_pubsub.cpp
// Test ZeroMQ pub/sub with Protocol Buffers
// Publisher: Sends synthetic market data
// Subscriber: Receives and displays market data

#include <iostream>
#include <thread>
#include <chrono>
#include <map>
#include "../zmq/market_data_server.hpp"
#include "../zmq/market_data_client.hpp"

using namespace hft;
using namespace std::chrono_literals;

// Publisher thread - sends synthetic market data
void publisher_thread() {
    std::this_thread::sleep_for(500ms); // Let subscriber connect first

    MarketDataServer server("tcp://*:5555");
    std::cout << "[PUBLISHER] Starting to send messages..." << std::endl;

    // Create synthetic order book data
    std::map<double, double> bids = {
        {50000.0, 1.5},
        {49999.0, 2.0},
        {49998.0, 1.0}
    };
    std::map<double, double> asks = {
        {50001.0, 1.0},
        {50002.0, 2.5},
        {50003.0, 3.0}
    };

    // Send snapshot
    std::cout << "[PUBLISHER] Sending snapshot..." << std::endl;
    server.publish_snapshot("BTC-USD", bids, asks, 0);
    std::this_thread::sleep_for(100ms);

    // Send updates
    for (int i = 0; i < 5; ++i) {
        std::cout << "[PUBLISHER] Sending update " << (i+1) << std::endl;
        
        std::vector<std::pair<double, double>> bid_changes = {{50000.0 + i, 1.5 + i*0.1}};
        std::vector<std::pair<double, double>> ask_changes = {{50001.0 + i, 1.0 + i*0.1}};
        
        server.publish_update("BTC-USD", bid_changes, ask_changes, i+1);
        std::this_thread::sleep_for(100ms);
    }

    // Send trade
    std::cout << "[PUBLISHER] Sending trade..." << std::endl;
    server.publish_trade("BTC-USD", 50000.5, 0.5, "buy", "trade123");
    std::this_thread::sleep_for(100ms);

    std::cout << "[PUBLISHER] Finished sending messages" << std::endl;
}

// Subscriber thread - receives market data
void subscriber_thread() {
    MarketDataClient client("tcp://localhost:5555");
    client.subscribe("BTC-USD");

    int snapshot_count = 0;
    int update_count = 0;
    int trade_count = 0;

    // Set up callbacks
    client.on_snapshot([&](const hft::OrderBookSnapshot& snap) {
        snapshot_count++;
        std::cout << "\n[SUBSCRIBER] Received SNAPSHOT #" << snapshot_count << std::endl;
        std::cout << "  Product: " << snap.product_id() << std::endl;
        std::cout << "  Sequence: " << snap.sequence() << std::endl;
        std::cout << "  Bids: " << snap.bids_size() << ", Asks: " << snap.asks_size() << std::endl;
        
        if (snap.bids_size() > 0) {
            std::cout << "  Best Bid: $" << snap.bids(0).price() << " x " << snap.bids(0).size() << std::endl;
        }
        if (snap.asks_size() > 0) {
            std::cout << "  Best Ask: $" << snap.asks(0).price() << " x " << snap.asks(0).size() << std::endl;
        }
    });

    client.on_update([&](const hft::OrderBookUpdate& upd) {
        update_count++;
        std::cout << "\n[SUBSCRIBER] Received UPDATE #" << update_count << std::endl;
        std::cout << "  Product: " << upd.product_id() << std::endl;
        std::cout << "  Sequence: " << upd.sequence() << std::endl;
        std::cout << "  Bid changes: " << upd.bid_changes_size() 
                  << ", Ask changes: " << upd.ask_changes_size() << std::endl;
    });

    client.on_trade([&](const hft::Trade& trade) {
        trade_count++;
        std::cout << "\n[SUBSCRIBER] Received TRADE #" << trade_count << std::endl;
        std::cout << "  Product: " << trade.product_id() << std::endl;
        std::cout << "  Price: $" << trade.price() << " x " << trade.size() << std::endl;
        std::cout << "  Side: " << trade.side() << std::endl;
    });

    std::cout << "[SUBSCRIBER] Waiting for messages..." << std::endl;

    // Receive messages for 2 seconds
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 2s) {
        client.receive_one();
    }

    std::cout << "\n[SUBSCRIBER] Summary:" << std::endl;
    std::cout << "  Snapshots: " << snapshot_count << std::endl;
    std::cout << "  Updates: " << update_count << std::endl;
    std::cout << "  Trades: " << trade_count << std::endl;
}

int main() {
    std::cout << "=== ZeroMQ Pub/Sub Test ===" << std::endl;
    std::cout << "Testing Protocol Buffer serialization over ZeroMQ" << std::endl << std::endl;

    // Start threads
    std::thread pub(publisher_thread);
    std::thread sub(subscriber_thread);

    // Wait for completion
    sub.join();
    pub.join();

    std::cout << "\n=== Test Complete ===" << std::endl;
    return 0;
}
