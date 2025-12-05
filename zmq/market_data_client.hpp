// filepath: /Users/israelbergenstein/Desktop/Desktop - Israel's MacBook Pro/MacBookWork/All_Desktop /Work/HFT/HFT_Coinbase/Coin_base_HFT/zmq/market_data_client.hpp
#pragma once
#include <zmq.hpp>       // ZeroMQ C++ bindings for network messaging
#include <string>        // For product IDs and endpoint strings
#include <functional>    // For std::function callbacks
#include <iostream>      // For logging subscription events
#include "../proto/messages.pb.h"  // Protobuf definitions for market data

namespace hft {

//=============================================================================
// MARKET DATA CLIENT: ZeroMQ Subscriber for Real-Time Market Data
//=============================================================================
// 
// PURPOSE:
// This class implements the SUBSCRIBER side of the ZeroMQ PUB/SUB pattern.
// It connects to a MarketDataServer and receives real-time market data updates:
// - Order book snapshots (full depth)
// - Order book updates (incremental changes)
// - Trade execution events
//
// ZEROMQ PUB/SUB PATTERN REFRESHER:
// ┌─────────────┐                    ┌─────────────┐
// │  Publisher  │ -----> Topic1 ---> │ Subscriber1 │
// │   (Server)  │ -----> Topic2 ---> │ Subscriber2 │
// └─────────────┘                    └─────────────┘
//
// - Publisher sends messages with topics (e.g., "BTC-USD", "ETH-USD")
// - Subscribers filter by topic prefix (subscribe to "BTC" matches "BTC-USD")
// - NO acknowledgments: Fire-and-forget, some messages may be lost
// - NO flow control: Subscribers must keep up or messages are dropped
//
// SUBSCRIBER PATTERN PROPERTIES:
// 1. **Late Joiner Problem**: Subscribers miss messages sent before connection
//    - Solution: Request a snapshot on startup
// 2. **Slow Subscriber Problem**: If subscriber can't keep up, messages queue
//    - Default high-water mark (HWM): 1000 messages, then drops
//    - Solution: Set subscriber_.set(zmq::sockopt::rcvhwm, 10000)
// 3. **No Reliability**: ZMQ PUB/SUB is best-effort, not guaranteed delivery
//    - For critical data, use REQ/REP or check sequence numbers
//
// CALLBACK PATTERN:
// Instead of polling "get_next_message()", this uses callbacks:
// - client.on_snapshot([](const Snapshot& s) { ... });
// - client.on_update([](const Update& u) { ... });
// - client.on_trade([](const Trade& t) { ... });
// Then call client.run() to enter event loop.
//
// ADVANTAGES:
// - Clean separation of concerns (networking vs business logic)
// - Easy to test: mock callbacks in unit tests
// - Flexible: same client can drive multiple strategies
//
// EXAMPLE USAGE:
// MarketDataClient client("tcp://localhost:5555");
// client.subscribe("BTC-USD");
// client.on_update([](const Update& u) {
//     std::cout << "Bid: " << u.bid_price() << std::endl;
// });
// client.run();  // Blocks forever, processing messages
//
// PERFORMANCE:
// - ZeroMQ SUB socket: ~50μs latency for TCP localhost
// - Protobuf deserialization: ~1-5μs for typical market data message
// - Total per-message latency: ~55-60μs (server → client → callback)
// - Throughput: Can handle 100,000+ messages/sec on modern hardware
//
// THREAD SAFETY:
// - ZeroMQ sockets are NOT thread-safe
// - Do NOT call receive_one() from multiple threads
// - If you need multi-threaded consumption:
//   Option 1: Use zmq::socket_t::monitor() to detect messages, then lock
//   Option 2: Use inproc:// sockets to fan out to multiple threads
//   Option 3: Create one MarketDataClient per thread, each subscribing
//
//=============================================================================
class MarketDataClient {
public:
    //=========================================================================
    // CALLBACK TYPE ALIASES
    //=========================================================================
    // std::function allows us to use lambdas, function pointers, or functors
    // These are called when messages of the corresponding type arrive
    using SnapshotCallback = std::function<void(const hft::OrderBookSnapshot&)>;
    using UpdateCallback = std::function<void(const hft::OrderBookUpdate&)>;
    using TradeCallback = std::function<void(const hft::Trade&)>;

    //=========================================================================
    // CONSTRUCTOR: Initialize ZeroMQ Context and Connect to Server
    //=========================================================================
    // endpoint: The address of the MarketDataServer
    //   - "tcp://localhost:5555" → Connect to server on same machine
    //   - "tcp://192.168.1.100:5555" → Connect to remote server
    //   - "ipc:///tmp/market_data.ipc" → Unix domain socket (faster, local only)
    //
    // ZeroMQ Context (zmq::context_t):
    // - Manages I/O threads for all sockets
    // - Parameter '1' means use 1 I/O thread
    // - For high-throughput systems (>100k msg/s), increase to 2-4 threads
    // - Context is expensive to create, so we create it once per client
    //
    // ZeroMQ SUB Socket (zmq::socket_type::sub):
    // - Subscriber socket that filters by topic prefix
    // - Must call subscribe() or subscribe_all() to receive ANY messages
    // - Default behavior: drops messages if no subscription is set
    //
    // IMPORTANT: subscriber_.connect() is NON-BLOCKING
    // - Returns immediately, connection happens asynchronously
    // - First few messages may be lost while connection establishes
    // - For production: add zmq::socket_t::monitor() to detect when connected
    //
    // EXAMPLE:
    // MarketDataClient client("tcp://192.168.1.100:5555");
    // // At this point, socket is connecting but not yet ready
    // std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // // Now likely connected
    MarketDataClient(const std::string& endpoint = "tcp://localhost:5555")
        : context_(1),                           // 1 I/O thread for ZeroMQ
          subscriber_(context_, zmq::socket_type::sub)  // Create SUB socket
    {
        // Connect to the publisher (non-blocking, asynchronous)
        subscriber_.connect(endpoint);
        std::cout << "Market Data Client connected to " << endpoint << std::endl;
    }

    //=========================================================================
    // SUBSCRIBE TO SPECIFIC PRODUCT
    //=========================================================================
    // product_id: The topic prefix to subscribe to (e.g., "BTC-USD")
    //
    // TOPIC FILTERING IN ZEROMQ:
    // - Subscription uses PREFIX matching, not exact match
    // - subscribe("BTC") matches "BTC-USD", "BTC-USDT", "BTCUSD", etc.
    // - subscribe("BTC-USD") matches ONLY "BTC-USD"
    // - Can call subscribe() multiple times to add more filters
    //
    // FILTERING HAPPENS ON PUBLISHER SIDE (important!):
    // - When you subscribe("BTC-USD"), the filter is sent to the server
    // - Server only sends messages where topic starts with "BTC-USD"
    // - Saves network bandwidth vs sending all messages and filtering locally
    //
    // EXAMPLE:
    // client.subscribe("BTC-USD");   // Get BTC-USD updates only
    // client.subscribe("ETH-USD");   // Also get ETH-USD updates
    // // Now receiving both BTC-USD and ETH-USD messages
    //
    // PERFORMANCE:
    // - Adding subscriptions is O(1), very fast
    // - No overhead for multiple subscriptions
    // - Can subscribe to 100s of products without performance hit
    //
    // UNSUBSCRIBE:
    // To remove a subscription:
    // subscriber_.set(zmq::sockopt::unsubscribe, "BTC-USD");
    void subscribe(const std::string& product_id) {
        // Set subscription filter on the socket
        subscriber_.set(zmq::sockopt::subscribe, product_id);
        std::cout << "Subscribed to: " << product_id << std::endl;
    }

    //=========================================================================
    // SUBSCRIBE TO ALL PRODUCTS
    //=========================================================================
    // Empty string "" means "match any topic" (no filtering)
    //
    // WHY USE subscribe_all() vs subscribe(""):
    // - They're identical: empty string matches everything
    // - subscribe_all() is just more readable/explicit
    //
    // WHEN TO USE:
    // - Monitoring dashboards that show all markets
    // - Cross-market correlation analysis
    // - Data archival systems
    //
    // WHEN NOT TO USE:
    // - Single-product strategies: wastes bandwidth/CPU
    // - High-frequency trading: process only what you need
    //
    // BANDWIDTH EXAMPLE:
    // If server publishes 100 products at 1000 updates/sec each:
    // - subscribe_all() receives 100,000 msg/s (all products)
    // - subscribe("BTC-USD") receives 1,000 msg/s (one product)
    // That's 100x difference in network traffic!
    void subscribe_all() {
        // Empty subscription filter → receive all messages
        subscriber_.set(zmq::sockopt::subscribe, "");
        std::cout << "Subscribed to all products" << std::endl;
    }

    //=========================================================================
    // SET CALLBACK FUNCTIONS
    //=========================================================================
    // These store callback functions to be invoked when messages arrive
    //
    // CALLBACK STORAGE:
    // - Callbacks are stored in member variables (snapshot_cb_, etc.)
    // - std::function can hold lambdas, function pointers, or functors
    // - If callback is not set, messages of that type are silently ignored
    //
    // LAMBDA EXAMPLE:
    // client.on_snapshot([](const OrderBookSnapshot& s) {
    //     std::cout << "Received snapshot for " << s.product_id() << std::endl;
    //     std::cout << "Best bid: " << s.bids(0).price() << std::endl;
    // });
    //
    // FUNCTION POINTER EXAMPLE:
    // void handle_update(const OrderBookUpdate& u) { ... }
    // client.on_update(handle_update);
    //
    // MEMBER FUNCTION EXAMPLE:
    // class MyStrategy {
    //     void on_trade(const Trade& t) { ... }
    // };
    // MyStrategy strat;
    // client.on_trade([&strat](const Trade& t) { strat.on_trade(t); });
    //
    // PERFORMANCE:
    // - std::function has small overhead (~1-2ns) vs direct function call
    // - Negligible compared to network latency (~50μs)
    void on_snapshot(SnapshotCallback cb) { snapshot_cb_ = cb; }
    void on_update(UpdateCallback cb) { update_cb_ = cb; }
    void on_trade(TradeCallback cb) { trade_cb_ = cb; }

    //=========================================================================
    // RECEIVE AND PROCESS ONE MESSAGE (BLOCKING)
    //=========================================================================
    // Returns: true if message received successfully, false on error
    //
    // ZEROMQ MULTIPART MESSAGE STRUCTURE:
    // Frame 1: Topic (e.g., "BTC-USD") - Used for subscription filtering
    // Frame 2: Protobuf data - The actual OrderBookSnapshot/Update/Trade
    //
    // WHY TWO FRAMES?
    // - ZeroMQ SUB sockets filter on first frame (topic) at network level
    // - This saves bandwidth: filtered messages never reach subscriber
    // - If we put topic inside protobuf, we'd receive all messages, then filter
    //
    // BLOCKING BEHAVIOR:
    // - recv() blocks until a message arrives
    // - No timeout by default (waits forever)
    // - To add timeout:
    //   subscriber_.set(zmq::sockopt::rcvtimeo, 1000);  // 1 sec timeout
    //   Then recv() returns false if no message in 1 second
    //
    // MESSAGE FLOW:
    // 1. recv() topic frame → Extract product ID (e.g., "BTC-USD")
    // 2. recv() data frame → Extract protobuf bytes
    // 3. ParseFromArray() → Deserialize protobuf to C++ object
    // 4. Check which message type (has_snapshot(), has_update(), has_trade())
    // 5. Dispatch to appropriate callback
    //
    // ERROR HANDLING:
    // - If recv() fails → return false (connection lost, context terminated)
    // - If ParseFromArray() fails → return false (corrupted data)
    // - If no callback set for message type → silently ignore
    //
    // EXAMPLE:
    // while (client.receive_one()) {
    //     // Process one message per iteration
    //     // Callbacks are invoked inside receive_one()
    // }
    // // Loop exits when receive fails (connection lost)
    //
    // PERFORMANCE:
    // - recv(): ~50μs for TCP localhost, <10μs for IPC
    // - ParseFromArray(): ~1-5μs for typical market data message
    // - Callback invocation: depends on your code
    // - Total: ~55-60μs per message
    bool receive_one() {
        //=====================================================================
        // STEP 1: Receive Topic Frame
        //=====================================================================
        // This is the first frame of the multipart message
        // Contains the product ID (e.g., "BTC-USD")
        zmq::message_t topic_msg;
        auto res = subscriber_.recv(topic_msg, zmq::recv_flags::none);
        
        // Check if receive failed
        // Reasons: connection lost, context terminated, signal interrupt
        if (!res) return false;

        // Extract topic string from message
        // topic_msg.data() returns void*, need to cast to char*
        // topic_msg.size() returns number of bytes
        std::string topic(static_cast<char*>(topic_msg.data()), topic_msg.size());

        //=====================================================================
        // STEP 2: Receive Data Frame (Protobuf)
        //=====================================================================
        // This is the second frame containing the actual market data
        // Serialized as Protobuf binary format
        zmq::message_t data_msg;
        res = subscriber_.recv(data_msg, zmq::recv_flags::none);
        
        // If first frame succeeded but second failed, something is very wrong
        // This should never happen in normal operation
        if (!res) return false;

        //=====================================================================
        // STEP 3: Deserialize Protobuf Message
        //=====================================================================
        // ParseFromArray() deserializes binary data into C++ object
        //
        // PROTOBUF PARSING:
        // - Reads binary format, validates field types
        // - Allocates memory for strings, repeated fields
        // - Returns false if data is corrupted or wrong message type
        //
        // PERFORMANCE:
        // - Protobuf is very fast: ~1-5μs for typical message
        // - Much faster than JSON (~50-100μs)
        // - Slightly slower than raw struct memcpy (~100ns)
        //
        // WHY PROTOBUF vs RAW STRUCTS?
        // - Forward/backward compatibility: add fields without breaking
        // - Cross-language: works with Python, Java, Go, etc.
        // - Compact: typically 30-50% smaller than JSON
        // - Type safety: can't accidentally misinterpret fields
        hft::MarketDataMessage msg;
        if (!msg.ParseFromArray(data_msg.data(), data_msg.size())) {
            std::cerr << "Failed to parse message" << std::endl;
            return false;
        }

        //=====================================================================
        // STEP 4: Dispatch to Appropriate Callback
        //=====================================================================
        // MarketDataMessage is a Protobuf "oneof" type:
        // message MarketDataMessage {
        //     oneof payload {
        //         OrderBookSnapshot snapshot = 1;
        //         OrderBookUpdate update = 2;
        //         Trade trade = 3;
        //     }
        // }
        //
        // Only ONE of snapshot/update/trade is set at a time
        // has_snapshot() returns true if snapshot field is populated
        //
        // CALLBACK INVOCATION:
        // - Check if callback is set (snapshot_cb_ != nullptr)
        // - If set, call it with the message data
        // - If not set, message is silently ignored
        //
        // WHY CHECK CALLBACK BEFORE CALLING?
        // - Prevents null pointer dereference
        // - Allows optional callbacks (only handle what you care about)
        //
        // EXAMPLE:
        // If you only care about trades:
        // client.on_trade([](const Trade& t) { ... });
        // // Snapshots and updates are ignored (not an error)
        if (msg.has_snapshot() && snapshot_cb_) {
            snapshot_cb_(msg.snapshot());
        } else if (msg.has_update() && update_cb_) {
            update_cb_(msg.update());
        } else if (msg.has_trade() && trade_cb_) {
            trade_cb_(msg.trade());
        }
        // If no callback is set for this message type, it's silently dropped
        // This is intentional: allows selective message processing

        return true;
    }

    //=========================================================================
    // RUN EVENT LOOP (BLOCKING FOREVER)
    //=========================================================================
    // This is the main event loop that processes messages indefinitely
    //
    // BLOCKING BEHAVIOR:
    // - Calls receive_one() in an infinite loop
    // - Each iteration blocks until a message arrives
    // - Only exits if receive_one() returns false (connection lost)
    //
    // WHEN TO USE run() vs receive_one():
    // - Use run() for standalone clients (e.g., data logger, monitoring)
    // - Use receive_one() when integrating into existing event loop
    //
    // EXAMPLE 1: Standalone Client
    // int main() {
    //     MarketDataClient client;
    //     client.subscribe_all();
    //     client.on_update([](const Update& u) { log_to_file(u); });
    //     client.run();  // Runs forever
    // }
    //
    // EXAMPLE 2: Integration with Existing Loop
    // while (running) {
    //     // Check for keyboard input
    //     if (kbhit()) handle_user_input();
    //     
    //     // Process one market data message (non-blocking)
    //     subscriber_.set(zmq::sockopt::rcvtimeo, 10);  // 10ms timeout
    //     client.receive_one();
    //     
    //     // Update strategy
    //     strategy.update();
    // }
    //
    // HOW TO EXIT THE LOOP:
    // Option 1: Set receive timeout + check flag
    //   subscriber_.set(zmq::sockopt::rcvtimeo, 1000);
    //   while (running) { receive_one(); }
    //
    // Option 2: Call zmq::context_t::shutdown() from another thread
    //   This makes all recv() calls fail immediately
    //
    // Option 3: Send SIGINT (Ctrl+C), handle in signal handler
    //   Set running=false, then join event loop thread
    //
    // PERFORMANCE:
    // - No overhead: just a while loop around receive_one()
    // - Can process 100,000+ messages/sec
    // - Latency: ~55-60μs per message (network + parsing + callback)
    void run() {
        std::cout << "Market Data Client event loop starting..." << std::endl;
        
        // Infinite loop: processes messages until receive fails
        while (true) {
            // Process one message (blocks until message arrives)
            if (!receive_one()) {
                // receive_one() returned false → error occurred
                // Possible reasons:
                // - Server disconnected
                // - Context terminated (zmq::context_t::shutdown())
                // - Signal interrupt (SIGINT)
                break;  // Exit event loop
            }
            // Callback was invoked inside receive_one()
            // Continue to next message
        }
        
        std::cout << "Market Data Client event loop exiting..." << std::endl;
    }

private:
    //=========================================================================
    // MEMBER VARIABLES
    //=========================================================================
    
    // ZeroMQ context: manages I/O threads for all sockets
    // - Created with 1 I/O thread (parameter in constructor)
    // - Shared by all sockets in this client
    // - Expensive to create, so we create it once
    zmq::context_t context_;
    
    // ZeroMQ SUB socket: subscribes to topics from publisher
    // - Filters messages by topic prefix
    // - Must call subscribe() to receive any messages
    // - NOT thread-safe: only call from one thread
    zmq::socket_t subscriber_;
    
    // Callback functions: invoked when messages arrive
    // - std::function can hold lambdas, function pointers, or functors
    // - If not set (default-constructed), these are "empty" (no callback)
    // - Check with if (snapshot_cb_) before calling
    SnapshotCallback snapshot_cb_;  // Called for full order book snapshots
    UpdateCallback update_cb_;      // Called for incremental updates
    TradeCallback trade_cb_;        // Called for trade executions
};

} // namespace hft
