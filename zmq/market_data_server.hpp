// zmq/market_data_server.hpp - ZeroMQ Publisher for Market Data Distribution
#pragma once

#include <zmq.hpp>       // ZeroMQ C++ bindings
#include <string>        // For topic strings
#include <memory>        // For smart pointers
#include <map>           // For orderbook data
#include <iostream>      // For logging
#include <chrono>        // For timestamps
#include "../proto/messages.pb.h"  // Protobuf message definitions

namespace hft {

/**
 * now_ns: Get current time in nanoseconds since epoch
 * 
 * WHY NANOSECONDS?
 * - Milliseconds: Too coarse for HFT (1ms = 1,000,000ns)
 * - Microseconds: Still too coarse (1μs = 1,000ns)
 * - Nanoseconds: Captures sub-microsecond timing
 * 
 * USE CASES:
 * - Timestamp market data messages
 * - Latency measurement
 * - Event ordering
 * 
 * PRECISION:
 * - std::chrono::system_clock: ~100ns resolution on modern systems
 * - Actual clock precision varies by hardware
 * 
 * @return Current time in nanoseconds since Unix epoch
 */
inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

/**
 * MarketDataServer: Publishes market data using ZeroMQ PUB/SUB pattern
 * 
 * PURPOSE:
 * - Decouple data collection from strategy execution
 * - Enable multiple strategies to consume same data stream
 * - Distribute data across multiple processes/machines
 * - Language-agnostic communication (Protobuf + ZMQ)
 * 
 * ZEROMQ PUB/SUB PATTERN:
 * 
 * Publisher (this class):
 * - Binds to an endpoint (e.g., tcp://*:5555)
 * - Sends messages with topic prefix
 * - Fire-and-forget (doesn't wait for subscribers)
 * 
 * Subscribers (separate processes):
 * - Connect to publisher endpoint
 * - Subscribe to topics (e.g., "BTC-USD", "ETH-USDT")
 * - Receive only messages matching their subscriptions
 * 
 * MESSAGE STRUCTURE:
 * Frame 1: Topic (string) - e.g., "BTC-USD"
 * Frame 2: Data (protobuf serialized) - OrderBookSnapshot, Update, or Trade
 * 
 * WHY PROTOBUF?
 * - Compact binary format (smaller than JSON)
 * - Fast serialization/deserialization
 * - Schema evolution (can add fields without breaking)
 * - Language-agnostic (C++, Python, Java, etc.)
 * 
 * PERFORMANCE:
 * - ZeroMQ: <10μs latency for local IPC
 * - TCP localhost: ~50μs latency
 * - TCP network: ~200μs latency
 * - Protobuf serialization: ~1-5μs
 * 
 * SCALABILITY:
 * - One publisher → thousands of subscribers
 * - No connection management overhead
 * - OS-level TCP fan-out
 */
class MarketDataServer {
public:
    /**
     * Constructor: Initialize ZeroMQ publisher socket
     * 
     * ALGORITHM:
     * 1. Create ZeroMQ context (thread pool, I/O threads)
     * 2. Create PUB socket
     * 3. Bind to endpoint
     * 
     * ENDPOINT FORMATS:
     * - tcp://*:5555 - TCP on all interfaces, port 5555
     * - tcp://127.0.0.1:5555 - TCP on localhost only
     * - ipc:///tmp/hft.ipc - Inter-process communication (Unix domain socket)
     * - inproc://hft - In-process (same process, different threads)
     * 
     * WHY tcp://*:5555?
     * - tcp: Works across machines (vs IPC which is local only)
     * - *: Listen on all network interfaces
     * - 5555: Arbitrary port number (choose something not in use)
     * 
     * BIND VS CONNECT:
     * - Publisher: BIND (server role)
     * - Subscriber: CONNECT (client role)
     * - One bind, many connects
     * 
     * @param endpoint: ZeroMQ endpoint to bind to
     */
    MarketDataServer(const std::string& endpoint = "tcp://*:5555")
        : context_(1),  // ZeroMQ context with 1 I/O thread
          publisher_(context_, zmq::socket_type::pub)  // PUB socket
    {
        // Bind to endpoint (start listening)
        publisher_.bind(endpoint);
        std::cout << "Market Data Server listening on " << endpoint << std::endl;
    }

    /**
     * publish_snapshot: Publish a full orderbook snapshot
     * 
     * USE CASE:
     * - Initial state when subscriber first connects
     * - Recovery after gap in sequence numbers
     * - Periodic full snapshots for reconciliation
     * 
     * ALGORITHM:
     * 1. Create OrderBookSnapshot protobuf message
     * 2. Set metadata (product_id, timestamp, sequence)
     * 3. Serialize bids in descending order (best bid first)
     * 4. Serialize asks in ascending order (best ask first)
     * 5. Wrap in MarketDataMessage
     * 6. Send via ZeroMQ
     * 
     * WHY SPECIFIC ORDERING?
     * - Bids descending: Highest price (best bid) first
     * - Asks ascending: Lowest price (best ask) first
     * - Matches exchange orderbook conventions
     * - Recipient can iterate to find best bid/ask efficiently
     * 
     * PROTOBUF REPEATED FIELDS:
     * - add_bids(): Appends a new bid level
     * - set_price(), set_size(): Set fields on that level
     * - Efficient: No intermediate copies
     * 
     * @param product_id: Trading pair (e.g., "BTC-USD")
     * @param bids: Bid side of orderbook (price → quantity)
     * @param asks: Ask side of orderbook (price → quantity)
     * @param sequence: Sequence number for gap detection
     */
    void publish_snapshot(const std::string& product_id,
                         const std::map<double, double>& bids,
                         const std::map<double, double>& asks,
                         uint64_t sequence) {
        // Create protobuf message
        hft::OrderBookSnapshot snapshot;
        snapshot.set_product_id(product_id);
        snapshot.set_timestamp_ns(now_ns());  // Current time in nanoseconds
        snapshot.set_sequence(sequence);      // For gap detection
        
        /**
         * SERIALIZE BIDS:
         * - std::map stores in ascending order by key (price)
         * - We want descending order (highest price first)
         * - Use reverse iterator: rbegin() → rend()
         * 
         * EXAMPLE:
         * bids: {50000: 1.0, 49999: 2.0, 49998: 0.5}
         * Output: [{50000, 1.0}, {49999, 2.0}, {49998, 0.5}]
         */
        for (auto it = bids.rbegin(); it != bids.rend(); ++it) {
            auto* level = snapshot.add_bids();  // Add new bid level
            level->set_price(it->first);        // Set price
            level->set_size(it->second);        // Set quantity
        }

        /**
         * SERIALIZE ASKS:
         * - std::map already in ascending order
         * - Use forward iterator
         * 
         * EXAMPLE:
         * asks: {50001: 0.5, 50002: 1.5, 50003: 2.0}
         * Output: [{50001, 0.5}, {50002, 1.5}, {50003, 2.0}]
         */
        for (const auto& [price, size] : asks) {
            auto* level = snapshot.add_asks();  // Add new ask level
            level->set_price(price);
            level->set_size(size);
        }

        /**
         * WRAP IN CONTAINER MESSAGE:
         * - MarketDataMessage is a "oneof" union type
         * - Can contain: snapshot, update, or trade
         * - This allows single subscriber to handle all message types
         */
        hft::MarketDataMessage msg;
        *msg.mutable_snapshot() = snapshot;  // Set snapshot field

        // Send via ZeroMQ
        send_message(product_id, msg);
    }

    /**
     * publish_update: Publish incremental orderbook changes
     * 
     * USE CASE:
     * - After initial snapshot, send only changes
     * - Reduces bandwidth (send 1 level vs 100 levels)
     * - Faster processing on subscriber side
     * 
     * ALGORITHM:
     * 1. Create OrderBookUpdate protobuf message
     * 2. Set metadata
     * 3. Serialize bid changes (price → new quantity)
     * 4. Serialize ask changes
     * 5. Wrap and send
     * 
     * CHANGE SEMANTICS:
     * - size > 0: Add or update level at this price
     * - size = 0: Remove level at this price
     * 
     * EXAMPLE:
     * bid_changes: [{50000, 1.5}, {49999, 0.0}]
     * Means: Update bid at 50000 to 1.5, remove bid at 49999
     * 
     * @param product_id: Trading pair
     * @param bid_changes: Changed bid levels
     * @param ask_changes: Changed ask levels
     * @param sequence: Sequence number
     */
    void publish_update(const std::string& product_id, 
                       const std::vector<std::pair<double, double>>& bid_changes,
                       const std::vector<std::pair<double, double>>& ask_changes,
                       uint64_t sequence) {
        // Create protobuf message
        hft::OrderBookUpdate update;
        update.set_product_id(product_id);
        update.set_timestamp_ns(now_ns());
        update.set_sequence(sequence);

        // Serialize bid changes
        for (const auto& [price, size] : bid_changes) {
            auto* level = update.add_bid_changes();
            level->set_price(price);
            level->set_size(size);  // 0 = remove, >0 = add/update
        }

        // Serialize ask changes
        for (const auto& [price, size] : ask_changes) {
            auto* level = update.add_ask_changes();
            level->set_price(price);
            level->set_size(size);
        }

        // Wrap in container message
        hft::MarketDataMessage msg;
        *msg.mutable_update() = update;

        send_message(product_id, msg);
    }

    /**
     * publish_trade: Publish a trade execution
     * 
     * USE CASE:
     * - Trade flow analysis
     * - VWAP calculation
     * - Volume tracking
     * - Tick data for charting
     * 
     * TRADE VS QUOTE:
     * - Quote: Someone places limit order (passive)
     * - Trade: Someone takes liquidity (aggressive)
     * 
     * SIDE INTERPRETATION:
     * - "buy": Taker bought (aggressor was buyer, price likely to rise)
     * - "sell": Taker sold (aggressor was seller, price likely to fall)
     * 
     * @param product_id: Trading pair
     * @param price: Execution price
     * @param size: Quantity traded
     * @param side: "buy" or "sell" (taker side)
     * @param trade_id: Unique trade identifier from exchange
     */
    void publish_trade(const std::string& product_id,
                      double price, double size, const std::string& side,
                      const std::string& trade_id) {
        // Create protobuf message
        hft::Trade trade;
        trade.set_product_id(product_id);
        trade.set_timestamp_ns(now_ns());
        trade.set_price(price);
        trade.set_size(size);
        trade.set_side(side);
        trade.set_trade_id(trade_id);

        // Wrap in container message
        hft::MarketDataMessage msg;
        *msg.mutable_trade() = trade;

        send_message(product_id, msg);
    }

private:
    /**
     * send_message: Internal method to send ZeroMQ message
     * 
     * ZEROMQ MULTIPART MESSAGES:
     * - Frame 1: Topic (used for subscription filtering)
     * - Frame 2: Data (protobuf serialized)
     * 
     * SEND FLAGS:
     * - sndmore: "More frames coming" (used for Frame 1)
     * - none: "Last frame" (used for Frame 2)
     * 
     * ALGORITHM:
     * 1. Serialize protobuf to binary string
     * 2. Create ZeroMQ message from topic string
     * 3. Send topic with sndmore flag
     * 4. Create ZeroMQ message from serialized data
     * 5. Send data with none flag (final frame)
     * 
     * TOPIC FILTERING:
     * - Subscriber can filter: "Give me only BTC-USD messages"
     * - Filtering happens on publisher side (reduces network traffic)
     * - Prefix matching: "BTC" matches "BTC-USD", "BTC-EUR", etc.
     * 
     * ERROR HANDLING:
     * - SerializeToString() returns false on error
     * - Rare: Only if message is malformed
     * - We log and return (don't throw, as this is hot path)
     * 
     * @param topic: Topic string (product_id)
     * @param msg: Protobuf message to send
     */
    void send_message(const std::string& topic, const hft::MarketDataMessage& msg) {
        /**
         * PROTOBUF SERIALIZATION:
         * - Converts in-memory structure to binary format
         * - Compact: Smaller than JSON
         * - Fast: ~1-5μs for typical market data message
         */
        std::string serialized;
        if (!msg.SerializeToString(&serialized)) {
            std::cerr << "Failed to serialize message" << std::endl;
            return;  // Don't throw - we're in hot path
        }

        /**
         * SEND TOPIC FRAME:
         * - zmq::message_t: Zero-copy wrapper around data
         * - sndmore flag: Indicates more frames follow
         */
        zmq::message_t topic_msg(topic.data(), topic.size());
        publisher_.send(topic_msg, zmq::send_flags::sndmore);

        /**
         * SEND DATA FRAME:
         * - Final frame (no sndmore flag)
         * - Subscriber receives both frames together
         */
        zmq::message_t data_msg(serialized.data(), serialized.size());
        publisher_.send(data_msg, zmq::send_flags::none);
    }

    /**
     * MEMBER VARIABLES:
     * 
     * context_: ZeroMQ context
     * - Manages background I/O threads
     * - Parameter 1: Number of I/O threads (1 is sufficient for most cases)
     * - Shared by all sockets in same process
     * 
     * publisher_: ZeroMQ PUB socket
     * - Sends messages to all connected subscribers
     * - No acknowledgments (fire-and-forget)
     * - No flow control (if subscriber is slow, messages are dropped)
     */
    zmq::context_t context_;   // ZeroMQ context (I/O thread pool)
    zmq::socket_t publisher_;  // PUB socket for broadcasting
};

/**
 * USAGE EXAMPLE:
 * 
 * ```cpp
 * // Create publisher
 * MarketDataServer server("tcp://*:5555");
 * 
 * // Publish snapshot
 * std::map<double, double> bids = {{50000, 1.0}, {49999, 2.0}};
 * std::map<double, double> asks = {{50001, 0.5}, {50002, 1.5}};
 * server.publish_snapshot("BTC-USD", bids, asks, 12345);
 * 
 * // Publish update
 * std::vector<std::pair<double, double>> bid_changes = {{50000, 1.5}};
 * std::vector<std::pair<double, double>> ask_changes = {{50001, 0.0}};
 * server.publish_update("BTC-USD", bid_changes, ask_changes, 12346);
 * 
 * // Publish trade
 * server.publish_trade("BTC-USD", 50000.5, 0.1, "buy", "trade-12345");
 * ```
 * 
 * SUBSCRIBER EXAMPLE (Python):
 * ```python
 * import zmq
 * 
 * context = zmq.Context()
 * subscriber = context.socket(zmq.SUB)
 * subscriber.connect("tcp://localhost:5555")
 * subscriber.setsockopt_string(zmq.SUBSCRIBE, "BTC-USD")
 * 
 * while True:
 *     topic, data = subscriber.recv_multipart()
 *     msg = MarketDataMessage()
 *     msg.ParseFromString(data)
 *     print(f"Received: {msg}")
 * ```
 * 
 * PRODUCTION CONSIDERATIONS:
 * - Monitor subscriber count (if 0, we're publishing to nobody)
 * - Handle slow subscribers (use HWM - High Water Mark)
 * - Consider multiple publishers for redundancy
 * - Use heartbeat messages to detect disconnections
 * - Log message rates for capacity planning
 */

} // namespace hft
