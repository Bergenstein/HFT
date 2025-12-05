// zmq/strategy_subscriber.hpp - Client that subscribes to market data feed
#pragma once
#include <zmq.hpp>            // ZeroMQ C++ bindings for network messaging
#include <string>             // For product IDs and topics
#include <functional>         // For std::function callbacks
#include <iostream>           // For logging
#include <thread>             // For background thread execution
#include <atomic>             // For thread-safe running flag
#include "../pipeline/normalized_data.hpp"  // NormalizedQuote, NormalizedTrade structs
#include "../proto/messages.pb.h"           // Protobuf message definitions

namespace hft {

//=============================================================================
// STRATEGY SUBSCRIBER: High-Level Market Data Consumer for Trading Strategies
//=============================================================================
//
// PURPOSE:
// This is a higher-level wrapper around MarketDataClient specifically designed
// for trading strategies. It provides:
// 1. Automatic conversion from Protobuf → NormalizedQuote/NormalizedTrade
// 2. Background thread support (run_async()) for non-blocking operation
// 3. Statistics tracking (quotes/trades received, errors)
// 4. Graceful shutdown mechanism
//
// DIFFERENCE vs MarketDataClient:
// ┌──────────────────────┬─────────────────────┬────────────────────────┐
// │ Feature              │ MarketDataClient    │ StrategySubscriber     │
// ├──────────────────────┼─────────────────────┼────────────────────────┤
// │ Data Format          │ Protobuf messages   │ Normalized structs     │
// │ Threading            │ Synchronous only    │ run() or run_async()   │
// │ Statistics           │ None                │ Built-in Stats         │
// │ Use Case             │ General purpose     │ Trading strategies     │
// │ Complexity           │ Lower level         │ Higher level           │
// └──────────────────────┴─────────────────────┴────────────────────────┘
//
// WHY NORMALIZED STRUCTS vs PROTOBUF?
// - Protobuf messages are complex: repeated fields, has_*() checks
// - NormalizedQuote is simple: just plain C++ struct with doubles
// - Easier to use in trading logic: quote.best_bid vs quote.bids(0).price()
// - Strategy code doesn't need to know about Protobuf
//
// THREADING MODEL:
// Option 1: Synchronous (run())
//   main_thread: strategy.run()
//   └─> Blocks forever, processes messages in strategy's thread
//
// Option 2: Asynchronous (run_async())
//   main_thread: strategy logic, order management
//   worker_thread: subscriber.run_async()
//   └─> Callbacks invoke strategy methods (must be thread-safe!)
//
// EXAMPLE USAGE (Synchronous):
// StrategySubscriber sub("tcp://localhost:5555");
// sub.subscribe("BTC-USD");
// sub.on_quote([](const NormalizedQuote& q) {
//     std::cout << "Bid: " << q.best_bid << "\n";
// });
// sub.run();  // Blocks forever
//
// EXAMPLE USAGE (Asynchronous):
// StrategySubscriber sub("tcp://localhost:5555");
// sub.subscribe_all();
// sub.on_quote([&strategy](const NormalizedQuote& q) {
//     strategy.on_market_data(q);  // Must be thread-safe!
// });
// sub.run_async();  // Returns immediately, runs in background
// // Do other work in main thread
// strategy.process_orders();
// // When done:
// sub.stop();
//
// PERFORMANCE:
// - Same as MarketDataClient: ~55-60μs per message
// - Additional overhead: ~1-2μs for Protobuf → Normalized conversion
// - Total latency: ~60-65μs (server → subscriber → callback)
//
// THREAD SAFETY:
// - Callbacks are invoked from worker thread (if run_async())
// - Your callback must be thread-safe if it accesses shared state
// - Use std::mutex or lock-free data structures in callbacks
//
//=============================================================================
class StrategySubscriber {
public:
    //=========================================================================
    // CONSTRUCTOR: Initialize ZeroMQ Subscriber
    //=========================================================================
    // endpoint: Server address (e.g., "tcp://localhost:5555")
    //
    // INITIALIZATION:
    // - context_(1): Create ZeroMQ context with 1 I/O thread
    // - subscriber_: Create SUB socket (must subscribe to receive messages)
    // - running_(false): Atomic flag for graceful shutdown
    //
    // ATOMIC BOOL (std::atomic<bool>):
    // - Thread-safe boolean flag
    // - Can be read/written from multiple threads without mutex
    // - Uses CPU atomic instructions (LOCK CMPXCHG on x86)
    // - Performance: ~1-2ns overhead vs regular bool
    //
    // WHY ATOMIC vs MUTEX?
    // - Atomic: Fast, lock-free, good for simple flags
    // - Mutex: Slower, but can protect complex state
    // - For running_ flag, atomic is perfect
    //
    // CONSTRUCTOR IS EXPLICIT:
    // - Prevents implicit conversions: StrategySubscriber s = "tcp://..."; ❌
    // - Must use: StrategySubscriber s("tcp://..."); ✅
    // - Best practice for single-argument constructors
    explicit StrategySubscriber(const std::string& endpoint = "tcp://localhost:5555")
        : context_(1),                                  // 1 I/O thread
          subscriber_(context_, zmq::socket_type::sub), // SUB socket
          running_(false)                               // Not running yet
    {
        // Connect to market data server (non-blocking)
        subscriber_.connect(endpoint);
        std::cout << "[SUBSCRIBER] Connected to " << endpoint << "\n";
    }
    
    //=========================================================================
    // DESTRUCTOR: Ensure Clean Shutdown
    //=========================================================================
    // RAII Pattern (Resource Acquisition Is Initialization):
    // - Constructor acquires resources (ZMQ socket, thread)
    // - Destructor releases resources (stop thread, close socket)
    // - Guarantees cleanup even if exception is thrown
    //
    // WHAT stop() DOES:
    // 1. Set running_ = false (signal worker thread to exit)
    // 2. Join worker thread (wait for it to finish)
    // 3. ZeroMQ socket is automatically closed by zmq::socket_t destructor
    //
    // WHY THIS MATTERS:
    // If we don't join the worker thread before destruction:
    // - Worker thread accesses member variables (subscriber_, callbacks_)
    // - Those variables are destroyed
    // - Worker thread crashes with segfault or use-after-free
    //
    // EXAMPLE FAILURE WITHOUT STOP:
    // {
    //     StrategySubscriber sub;
    //     sub.run_async();
    //     // sub goes out of scope → destructor called
    //     // worker_thread_ is still running!
    //     // worker_thread_ tries to access subscriber_ → CRASH
    // }
    ~StrategySubscriber() {
        stop();  // Always stop worker thread before destruction
    }
    
    //=========================================================================
    // SUBSCRIBE TO SPECIFIC PRODUCT
    //=========================================================================
    // See MarketDataClient::subscribe() for detailed explanation
    // Topic filtering happens on server side, saves network bandwidth
    void subscribe(const std::string& product_id) {
        subscriber_.set(zmq::sockopt::subscribe, product_id);
        std::cout << "[SUBSCRIBER] Subscribed to: " << product_id << "\n";
    }
    
    //=========================================================================
    // SUBSCRIBE TO ALL PRODUCTS
    //=========================================================================
    // Empty string "" matches all topics
    // Useful for multi-product strategies or monitoring dashboards
    void subscribe_all() {
        subscriber_.set(zmq::sockopt::subscribe, "");
        std::cout << "[SUBSCRIBER] Subscribed to ALL products\n";
    }
    
    //=========================================================================
    // REGISTER CALLBACK FOR QUOTES
    //=========================================================================
    // callback: Function to call when quote (snapshot/update) arrives
    //
    // NORMALIZED QUOTE:
    // struct NormalizedQuote {
    //     std::string product_id;
    //     double best_bid, best_ask;
    //     double bid_size, ask_size;
    //     std::vector<PriceLevel> bids, asks;  // Top 5 levels
    //     uint64_t sequence;
    //     TimePoint local_timestamp;
    // };
    //
    // std::move() OPTIMIZATION:
    // - std::function contains function pointer + captured variables
    // - Copy is expensive if lambda captures large objects
    // - std::move() transfers ownership instead of copying
    // - Performance: ~10-20ns saved vs copy
    //
    // EXAMPLE:
    // sub.on_quote([](const NormalizedQuote& q) {
    //     double spread = q.best_ask - q.best_bid;
    //     if (spread < 0.01) { /* trade */ }
    // });
    void on_quote(std::function<void(const pipeline::NormalizedQuote&)> callback) {
        quote_callback_ = std::move(callback);
    }
    
    //=========================================================================
    // REGISTER CALLBACK FOR TRADES
    //=========================================================================
    // callback: Function to call when trade message arrives
    //
    // NORMALIZED TRADE:
    // struct NormalizedTrade {
    //     std::string product_id;
    //     double price, size;
    //     std::string side;  // "buy" or "sell"
    //     std::string trade_id;
    //     TimePoint timestamp;
    // };
    //
    // QUOTES vs TRADES:
    // - Quotes: Passive liquidity (resting orders in book)
    // - Trades: Aggressive liquidity (market orders executed)
    // - Quotes update 100x more frequently than trades
    // - Both are important: quotes for spread, trades for volume/direction
    void on_trade(std::function<void(const pipeline::NormalizedTrade&)> callback) {
        trade_callback_ = std::move(callback);
    }
    
    //=========================================================================
    // RUN SYNCHRONOUS EVENT LOOP (BLOCKING)
    //=========================================================================
    // Processes messages in the current thread until stop() is called
    //
    // BLOCKING BEHAVIOR:
    // - Sets running_ = true
    // - Loops forever: receive message → deserialize → invoke callback
    // - Only exits when running_ becomes false (via stop())
    //
    // MESSAGE PROCESSING FLOW:
    // 1. recv() topic frame (product ID)
    // 2. recv() data frame (protobuf bytes)
    // 3. ParseFromArray() → MarketDataMessage
    // 4. Check message type (snapshot/update/trade)
    // 5. Convert to NormalizedQuote/NormalizedTrade
    // 6. Invoke callback
    //
    // WHY recv_flags::none vs recv_flags::dontwait?
    // - recv_flags::none: BLOCKING - waits for message (what we want)
    // - recv_flags::dontwait: NON-BLOCKING - returns immediately if no message
    // - For event loop, we want to block (sleep) until message arrives
    //
    // ATOMIC OPERATIONS:
    // - running_.load(): Read atomic boolean (sequentially consistent)
    // - running_.store(): Write atomic boolean (sequentially consistent)
    // - Memory ordering: Default is std::memory_order_seq_cst (safest, slowest)
    // - Could optimize to memory_order_relaxed (~1ns faster) but not worth complexity
    //
    // ERROR HANDLING:
    // - recv() fails → continue (try next message)
    // - ParseFromArray() fails → continue (skip corrupted message)
    // - Callback throws exception → propagates up, crashes loop (by design)
    //
    // EXAMPLE:
    // sub.on_quote([](const NormalizedQuote& q) {
    //     std::cout << "Received quote\n";
    // });
    // sub.run();  // Blocks here until stop() is called
    void run() {
        running_.store(true);  // Atomic write: signal we're running
        std::cout << "[SUBSCRIBER] Starting message loop...\n";
        
        // Main event loop
        while (running_.load()) {  // Atomic read: check if still running
            zmq::message_t topic_msg;
            zmq::message_t data_msg;
            
            //=================================================================
            // STEP 1: Receive Topic Frame (Product ID)
            //=================================================================
            // First frame of multipart message
            // Contains product ID like "BTC-USD"
            auto res1 = subscriber_.recv(topic_msg, zmq::recv_flags::none);
            if (!res1) continue;  // recv() failed, skip this iteration
            
            //=================================================================
            // STEP 2: Receive Data Frame (Protobuf)
            //=================================================================
            // Second frame containing serialized MarketDataMessage
            auto res2 = subscriber_.recv(data_msg, zmq::recv_flags::none);
            if (!res2) continue;  // recv() failed, skip this iteration
            
            // Extract topic string from first frame
            std::string topic(static_cast<char*>(topic_msg.data()), topic_msg.size());
            
            //=================================================================
            // STEP 3: Deserialize Protobuf Message
            //=================================================================
            // Convert binary data → C++ Protobuf object
            hft::MarketDataMessage msg;
            if (!msg.ParseFromArray(data_msg.data(), data_msg.size())) {
                std::cerr << "[SUBSCRIBER] Failed to parse message\n";
                continue;  // Corrupted data, skip
            }
            
            //=================================================================
            // STEP 4: Dispatch to Message Type Handler
            //=================================================================
            // MarketDataMessage is a "oneof" type:
            // - has_snapshot() → Full order book
            // - has_update() → Incremental update
            // - has_trade() → Trade execution
            //
            // PROCESS FUNCTIONS:
            // - process_snapshot(): Converts Protobuf → NormalizedQuote
            // - process_update(): Converts Protobuf → NormalizedQuote
            // - process_trade(): Converts Protobuf → NormalizedTrade
            // - Then invokes callbacks (quote_callback_ or trade_callback_)
            if (msg.has_snapshot() && quote_callback_) {
                process_snapshot(msg.snapshot());
            } else if (msg.has_update() && quote_callback_) {
                process_update(msg.update());
            } else if (msg.has_trade() && trade_callback_) {
                process_trade(msg.trade());
            }
            // If no callback registered, message is silently ignored
        }
        
        std::cout << "[SUBSCRIBER] Stopped\n";
    }
    
    //=========================================================================
    // RUN ASYNCHRONOUS EVENT LOOP (NON-BLOCKING)
    //=========================================================================
    // Starts run() in a background thread, returns immediately
    //
    // THREADING:
    // - std::thread: C++11 thread abstraction
    // - Lambda capture [this]: Captures 'this' pointer to call run()
    // - worker_thread_ stores the thread object
    //
    // THREAD LIFETIME:
    // 1. run_async() creates thread → thread starts executing run()
    // 2. Main thread continues doing other work
    // 3. Worker thread runs until running_ becomes false
    // 4. stop() sets running_ = false, then joins thread
    //
    // THREAD SAFETY WARNING:
    // - Callbacks execute in worker thread, not main thread
    // - If callbacks access shared state, you MUST synchronize
    // - Use std::mutex, std::atomic, or lock-free queues
    //
    // EXAMPLE WITH SHARED STATE (WRONG):
    // int counter = 0;  // NOT thread-safe!
    // sub.on_quote([&counter](const NormalizedQuote& q) {
    //     counter++;  // Race condition!
    // });
    // sub.run_async();
    // std::cout << counter << "\n";  // Race condition!
    //
    // EXAMPLE WITH SHARED STATE (CORRECT):
    // std::atomic<int> counter{0};  // Thread-safe!
    // sub.on_quote([&counter](const NormalizedQuote& q) {
    //     counter.fetch_add(1);  // Atomic increment
    // });
    // sub.run_async();
    // std::this_thread::sleep_for(1s);
    // std::cout << counter.load() << "\n";  // Thread-safe read
    //
    // ALTERNATIVE: Use mutex
    // std::mutex mtx;
    // int counter = 0;
    // sub.on_quote([&mtx, &counter](const NormalizedQuote& q) {
    //     std::lock_guard<std::mutex> lock(mtx);
    //     counter++;
    // });
    void run_async() {
        // Create worker thread that executes run()
        // Lambda [this]() { run(); } captures 'this' and calls member function
        worker_thread_ = std::thread([this]() { run(); });
        
        // Returns immediately - run() is executing in background
        // Main thread can continue doing other work
    }
    
    //=========================================================================
    // STOP THE SUBSCRIBER (GRACEFUL SHUTDOWN)
    //=========================================================================
    // Signals worker thread to exit and waits for it to finish
    //
    // SHUTDOWN SEQUENCE:
    // 1. Check if running (avoid double-stop)
    // 2. Set running_ = false (worker thread sees this in while condition)
    // 3. Worker thread exits run() loop
    // 4. join() waits for worker thread to finish
    // 5. Thread is cleaned up
    //
    // WHY joinable() CHECK?
    // - Can only join() a thread once
    // - If already joined, joinable() returns false
    // - Calling join() on non-joinable thread → std::system_error exception
    //
    // BLOCKING BEHAVIOR:
    // - join() BLOCKS until worker thread finishes
    // - If worker thread is stuck in recv(), this blocks forever
    // - Solution: Set recv timeout before calling stop():
    //   subscriber_.set(zmq::sockopt::rcvtimeo, 1000);  // 1 sec timeout
    //
    // ALTERNATIVE SHUTDOWN (Non-blocking):
    // Instead of join(), use detach():
    // worker_thread_.detach();  // Thread runs independently
    // BUT: Dangerous! Thread may access destroyed objects
    //
    // EXAMPLE:
    // sub.run_async();
    // std::this_thread::sleep_for(std::chrono::seconds(10));
    // sub.stop();  // Waits for worker thread to finish
    void stop() {
        if (running_.load()) {  // Check if currently running
            running_.store(false);  // Signal worker thread to exit
            
            // Wait for worker thread to finish
            if (worker_thread_.joinable()) {
                worker_thread_.join();  // Blocks until thread exits
            }
        }
        // Now safe to destroy StrategySubscriber
    }
    
    //=========================================================================
    // GET STATISTICS
    //=========================================================================
    // Returns struct with message counts and error counts
    //
    // STATISTICS TRACKING:
    // - quotes_received: Snapshots + Updates
    // - trades_received: Trade executions
    // - errors: Currently unused (could track parse failures)
    //
    // THREAD SAFETY:
    // - Stats are updated in worker thread (process_snapshot/update/trade)
    // - Stats are read in main thread (get_stats())
    // - This is a RACE CONDITION! (but probably benign)
    //
    // WHY NOT THREAD-SAFE?
    // - Making Stats atomic adds overhead
    // - Stats are approximate, don't need exact precision
    // - For exact counts, use std::atomic<uint64_t>
    //
    // FIXING RACE CONDITION (if needed):
    // struct Stats {
    //     std::atomic<uint64_t> quotes_received{0};
    //     std::atomic<uint64_t> trades_received{0};
    //     std::atomic<uint64_t> errors{0};
    // };
    // Then: stats_.quotes_received.fetch_add(1);
    //
    // EXAMPLE:
    // auto stats = sub.get_stats();
    // std::cout << "Quotes: " << stats.quotes_received << "\n";
    // std::cout << "Trades: " << stats.trades_received << "\n";
    struct Stats {
        uint64_t quotes_received = 0;  // Total quotes processed
        uint64_t trades_received = 0;  // Total trades processed
        uint64_t errors = 0;           // Total errors (unused currently)
    };
    
    Stats get_stats() const { return stats_; }

private:
    //=========================================================================
    // PROCESS SNAPSHOT: Convert Protobuf Snapshot → NormalizedQuote
    //=========================================================================
    // snapshot: Full order book from Protobuf message
    //
    // ORDER BOOK SNAPSHOT:
    // Contains complete state of order book at a point in time:
    // - All bids: [{price: 50000, size: 1.5}, {price: 49999, ...}, ...]
    // - All asks: [{price: 50001, size: 2.0}, {price: 50002, ...}, ...]
    // - Sequence number: For detecting missed updates
    //
    // CONVERSION LOGIC:
    // 1. Extract product_id, sequence from Protobuf
    // 2. Extract best bid/ask (first element of bids/asks arrays)
    // 3. Store top 5 levels (for depth analysis)
    // 4. Record local timestamp (when we received it)
    // 5. Invoke quote callback
    //
    // WHY TOP 5 LEVELS?
    // - Strategies need order book depth for:
    //   - Liquidity analysis: How much size at each level?
    //   - Support/resistance: Are there large walls?
    //   - Market impact: What's the cost of a large order?
    // - 5 levels is typical for HFT (some use 10-20)
    // - Full book (100+ levels) is overkill for most strategies
    //
    // PERFORMANCE:
    // - std::min(5, snapshot.bids_size()): Prevents out-of-bounds access
    // - Loop 5 times: ~20-50ns total
    // - push_back(): Amortized O(1), no reallocation if reserved
    // - Total conversion: ~1-2μs
    //
    // EXAMPLE SNAPSHOT:
    // product_id: "BTC-USD"
    // sequence: 12345678
    // bids: [{50000, 1.5}, {49999, 2.0}, {49998, 1.0}, ...]
    // asks: [{50001, 2.0}, {50002, 1.5}, {50003, 3.0}, ...]
    //
    // NORMALIZED QUOTE OUTPUT:
    // best_bid: 50000, bid_size: 1.5
    // best_ask: 50001, ask_size: 2.0
    // spread: 1.0
    // bids: [{50000, 1.5}, {49999, 2.0}, {49998, 1.0}, {49997, 0.5}, {49996, 2.5}]
    // asks: [{50001, 2.0}, {50002, 1.5}, {50003, 3.0}, {50004, 1.0}, {50005, 0.8}]
    void process_snapshot(const hft::OrderBookSnapshot& snapshot) {
        pipeline::NormalizedQuote quote;
        quote.product_id = snapshot.product_id();
        quote.sequence = snapshot.sequence();
        
        //=====================================================================
        // Extract Bids (Buy Orders)
        //=====================================================================
        // bids_size() returns number of bid levels in Protobuf repeated field
        // bids(0) returns first bid (best bid - highest price)
        // Bids are sorted descending: [50000, 49999, 49998, ...]
        if (snapshot.bids_size() > 0) {
            quote.best_bid = snapshot.bids(0).price();
            quote.bid_size = snapshot.bids(0).size();
            
            // Store top 5 bid levels for depth analysis
            for (int i = 0; i < std::min(5, snapshot.bids_size()); ++i) {
                quote.bids.push_back({
                    snapshot.bids(i).price(),
                    snapshot.bids(i).size()
                });
            }
        }
        
        //=====================================================================
        // Extract Asks (Sell Orders)
        //=====================================================================
        // asks(0) returns first ask (best ask - lowest price)
        // Asks are sorted ascending: [50001, 50002, 50003, ...]
        if (snapshot.asks_size() > 0) {
            quote.best_ask = snapshot.asks(0).price();
            quote.ask_size = snapshot.asks(0).size();
            
            // Store top 5 ask levels for depth analysis
            for (int i = 0; i < std::min(5, snapshot.asks_size()); ++i) {
                quote.asks.push_back({
                    snapshot.asks(i).price(),
                    snapshot.asks(i).size()
                });
            }
        }
        
        // Record when we received this message (for latency measurement)
        quote.local_timestamp = std::chrono::system_clock::now();
        
        // Update statistics
        stats_.quotes_received++;
        
        // Invoke callback if registered
        if (quote_callback_) {
            quote_callback_(quote);
        }
    }
    
    //=========================================================================
    // PROCESS UPDATE: Convert Protobuf Update → NormalizedQuote
    //=========================================================================
    // update: Incremental order book change
    //
    // INCREMENTAL UPDATES:
    // Instead of sending full snapshot every time, send only changes:
    // - "Add order at price 50000, size 1.5"
    // - "Remove order at price 49999"
    // - "Modify order at price 50001, new size 0.5"
    //
    // ADVANTAGES:
    // - Bandwidth: 10-100x less data than snapshots
    // - Latency: Smaller messages → faster transmission
    // - CPU: Less parsing overhead
    //
    // DISADVANTAGES:
    // - Complexity: Must maintain order book state
    // - Reliability: If you miss one update, book is desynchronized
    // - Solution: Use sequence numbers, request snapshot if gap detected
    //
    // CURRENT IMPLEMENTATION (SIMPLIFIED):
    // - Just extracts product_id and sequence
    // - Doesn't fully populate best_bid/best_ask (would need order book)
    // - For full implementation, maintain OrderBook state and apply updates
    //
    // PRODUCTION IMPLEMENTATION:
    // class StrategySubscriber {
    //     std::unordered_map<std::string, OrderBook> books_;
    //     
    //     void process_update(const OrderBookUpdate& update) {
    //         auto& book = books_[update.product_id()];
    //         book.apply_update(update);  // Modify internal state
    //         quote.best_bid = book.get_best_bid();
    //         quote.best_ask = book.get_best_ask();
    //     }
    // };
    void process_update(const hft::OrderBookUpdate& update) {
        // Similar to snapshot but for incremental updates
        // In production, would apply update to maintained OrderBook state
        pipeline::NormalizedQuote quote;
        quote.product_id = update.product_id();
        quote.sequence = update.sequence();
        quote.local_timestamp = std::chrono::system_clock::now();
        
        // Note: This is simplified - full implementation would:
        // 1. Look up existing OrderBook for this product
        // 2. Apply update (add/remove/modify price levels)
        // 3. Extract new best_bid/best_ask from updated book
        
        stats_.quotes_received++;
        if (quote_callback_) {
            quote_callback_(quote);
        }
    }
    
    //=========================================================================
    // PROCESS TRADE: Convert Protobuf Trade → NormalizedTrade
    //=========================================================================
    // trade_msg: Trade execution from exchange
    //
    // TRADE INFORMATION:
    // - product_id: Which market (e.g., "BTC-USD")
    // - price: Execution price
    // - size: Execution size (in base currency)
    // - side: "buy" or "sell" (aggressor side)
    // - trade_id: Unique identifier
    // - timestamp: When trade occurred (exchange time)
    //
    // AGGRESSOR SIDE:
    // - "buy": Buyer was aggressor (market buy order)
    // - "sell": Seller was aggressor (market sell order)
    // - Important for order flow analysis:
    //   - Many "buy" trades → Buying pressure
    //   - Many "sell" trades → Selling pressure
    //
    // TRADE vs QUOTE USAGE:
    // - Quotes: Predict future price (where is liquidity?)
    // - Trades: Confirm activity (what actually happened?)
    // - Strategies use both: Quotes for entries, trades for confirmations
    //
    // EXAMPLE:
    // product_id: "BTC-USD"
    // price: 50000.5
    // size: 0.1 BTC
    // side: "buy" (buyer paid $50000.5 per BTC)
    // trade_id: "12345678"
    //
    // INTERPRETATION:
    // - Aggressive buyer purchased 0.1 BTC at $50000.5
    // - They "crossed the spread" (paid the ask price)
    // - This removes liquidity from order book
    void process_trade(const hft::Trade& trade_msg) {
        pipeline::NormalizedTrade trade;
        trade.product_id = trade_msg.product_id();
        trade.price = trade_msg.price();
        trade.size = trade_msg.size();
        trade.side = trade_msg.side();
        trade.trade_id = trade_msg.trade_id();
        trade.timestamp = std::chrono::system_clock::now();
        
        stats_.trades_received++;
        if (trade_callback_) {
            trade_callback_(trade);
        }
    }
    
    //=========================================================================
    // MEMBER VARIABLES
    //=========================================================================
    
    zmq::context_t context_;       // ZeroMQ I/O context (manages threads)
    zmq::socket_t subscriber_;     // ZeroMQ SUB socket (receives messages)
    std::atomic<bool> running_;    // Thread-safe flag: is event loop running?
    std::thread worker_thread_;    // Background thread for run_async()
    
    // Callbacks invoked when messages arrive
    std::function<void(const pipeline::NormalizedQuote&)> quote_callback_;
    std::function<void(const pipeline::NormalizedTrade&)> trade_callback_;
    
    Stats stats_;  // Message counters (not thread-safe, but benign)
};

} // namespace hft
