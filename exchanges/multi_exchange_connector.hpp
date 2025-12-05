// exchanges/multi_exchange_connector.hpp
// Multi-Exchange WebSocket Connector with Queue Integration
// exchanges/multi_exchange_connector.hpp
// Multi-Exchange WebSocket Connector with Queue Integration
/*
Detailed module-level documentation (purpose + concurrency notes):

This header exposes a small set of exchange adapters (Coinbase, Binance, Kraken (disabled))
and a simple "MultiExchangeConnector" to manage multiple adapters via threads.

High-level pipeline:
    - Each exchange adapter connects to its exchange via WebSocket(s).
    - Messages received are parsed into exchange-specific JSON.
    - A shared normalizer converts exchange JSON into pipeline::NormalizedQuote.
    - The adapter pushes the NormalizedQuote into a pipeline queue (queue_).
    - Downstream consumer(s) read NormalizedQuote from the queue and process them.

Important concurrency and queue-type considerations:
    - The pipeline::SPSCQueue<T> stands for Single-Producer, Single-Consumer.
        It is usually implemented as a lock-free ring buffer and is only safe when:
            * Exactly one *producer* thread is writing to it, and
            * Exactly one *consumer* thread is reading from it.
        If multiple producers push concurrently to an SPSC queue, behavior is undefined
        and will result in data races, memory corruption, or lost writes.

    - Current implementation detail:
        The application constructs one queue and passes a shared_ptr to it to all connectors.
        Then MultiExchangeConnector launches multiple threads (one per exchange or symbol).
        Those threads call queue_->try_push(...) concurrently whenever they get messages.
        This is unsafe if using pipeline::SPSCQueue because there can be multiple producers.

    - Options for fixing the multi-producer problem:
        1) Use a multi-producer queue (MPMC or at least MPSC).
             - Replace pipeline::SPSCQueue<T> with an MPMC implementation (e.g., moodycamel::ConcurrentQueue,
                 boost::lockfree::queue, folly::ProducerToken + ConcurrentQueue, or your own MPMC queue).
             - Pros: Minimal changes to threading model (multiple threads keep pushing directly).
             - Cons: MPMC implementations can be slightly slower than SPSC due to additional synchronization,
                 but tradeoff is often acceptable for multiple producers.

        2) Keep SPSC per-producer and aggregate:
             - Give each producer its own SPSC queue (owned by that producer and a single consumer aggregator).
             - Launch a single aggregator (single-producer that consumes all SPSC queues and re-publishes to
                 the downstream SPSC queue that the application expects).
             - Pros: Preserve lock-free SPSC performance on each producer; natural message partitioning.
             - Cons: More complexity (more queues + aggregator thread). Latency may increase slightly.

        3) Use ZeroMQ to decouple producers/consumers:
             - Each connector pushes via a ZeroMQ PUSH socket to an inproc/ipc/tcp listener.
             - A ZeroMQ PULL socket receives messages and forwards them to the consumer (or a single consumer).
             - Pros: ZeroMQ handles concurrency, queuing, message batching, and reconnection; also helpful for
                 multi-process setups.
             - Cons: Adds a dependency and IPC overhead; design choice depends on deployment.

    - Backpressure & try_push:
        - This code uses queue_->try_push(quote) (non-blocking).
            * try_push typically returns false on a full queue -> the code silently drops the quote.
            * This is acceptable for best-effort use cases (e.g., high-frequency streaming analytics),
                but not acceptable for lossless, guaranteed delivery.
        - If data loss is unacceptable, either:
            * Use a blocking push/push_wait method (and tune queue depth), or
            * Implement an explicit backpressure policy: drop only low-value messages, or buffer, or persist.
        - Example: If you adopt an MPMC queue with blocking push semantics, add a small backoff
            or bounded queue with monitoring/metrics.

Threading model notes:
    - CoinbaseConnector: uses one WebSocket connection and reads snapshots from a single thread (single-producer safe).
    - BinanceConnector: starts a dedicated thread per symbol (multiple producers if you pass the same queue).
    - KrakenConnector: disabled (websocketpp compatibility issue with Boost 1.86+).
    - MultiExchangeConnector: spawns a thread per exchange adaptor; those threads create further worker threads
        (Binance creates one thread per symbol), so be cautious with the total number of threads.

Safety & lifecycle:
    - If MultiExchangeConnector::stop() is called, it sets `running_` to false, but the Exchange connector run loops
        do not check this `running_` flag. For graceful shutdown:
            * Pass an atomic stop token to connectors, or
            * Implement socket cancellation and cooperative shutdown in each run() method.
    - Error handling: Many operations log and throw or swallow exceptions. Implement a retry/reconnect strategy with
        exponential backoff for robust production behavior. Also watch for memory/exceptions thrown in threads.

Performance & resource considerations:
    - Per-symbol threads increase context switching and memory footprint. Consider a pooled/event loop
        approach to multiplex many subscriptions over fewer threads using asynchronous IO.
    - Serialization costs:
            * Normalizer returns pipeline::NormalizedQuote by value; consider move semantics or in-place construction
                if heavy. Also ensure the NormalizedQuote is trivially copyable or sized appropriately for your queue.

Security & TLS:
    - Coinbase uses Boost.Beast + OpenSSL; currently SSL verification is disabled (ctx.set_verify_mode(... verify_none)).
        For production, enable proper certificate verification and SNI, and manage root store appropriately.

Note on message types:
    - This code processes Coinbase 'snapshot' events only; incremental updates are ignored.
        For a correct orderbook, you likely need to handle snapshot + incremental updates with sequence numbers and replay.
*/

#pragma once

// Standard headers used for threading, IO, memory management
#include <iostream>     // logging via std::cout / std::cerr
#include <thread>       // std::thread
#include <atomic>       // std::atomic
#include <memory>       // std::shared_ptr
#include <vector>       // std::vector
#include <string>       // std::string

// JSON parsing used to parse exchange messages
#include <nlohmann/json.hpp>

// Pipeline includes - MPMC queue for multi-producer support, NormalizedQuote type, and normalizer
// DESIGN DECISION: Using MPMCQueue because MultiExchangeConnector spawns MULTIPLE producer threads
// (one per exchange: Coinbase, Binance, Kraken) that all push to the SAME queue.
// SPSCQueue would cause undefined behavior with multiple producers.
#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalized_data.hpp"
#include "../pipeline/normalizer.hpp"

// Exchange-specific websocket client implementation for Binance; this abstraction hides the websocket details.
#include "binance_ws_client.hpp"
// NOTE: Kraken uses websocketpp which has compatibility issues with newer Boost
// We'll add it later after fixing the compatibility
// #include "kraken_ws_client.hpp"

namespace exchanges {

using json = nlohmann::json;

// Coinbase adapter that pushes to queue
class CoinbaseConnector {
public:
    CoinbaseConnector(const std::vector<std::string>& products,
                      std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> queue)
        : products_(products), queue_(queue) {}
    
    void run() {
        try {
            const std::string host = "advanced-trade-ws.coinbase.com";
            const std::string port = "443";
            
            boost::asio::io_context ioc;
            boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_client};
            ctx.set_verify_mode(boost::asio::ssl::verify_none);
            
            boost::asio::ip::tcp::resolver resolver{ioc};
            boost::beast::websocket::stream<boost::beast::ssl_stream<boost::asio::ip::tcp::socket>> ws{ioc, ctx};
            
            auto const results = resolver.resolve(host, port);
            boost::asio::connect(ws.next_layer().next_layer(), results.begin(), results.end());
            
            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
                boost::beast::error_code ec{static_cast<int>(::ERR_get_error()), 
                                           boost::asio::error::get_ssl_category()};
                throw boost::beast::system_error{ec, "SNI failed"};
            }
            
            ws.next_layer().handshake(boost::asio::ssl::stream_base::client);
            ws.set_option(boost::beast::websocket::stream_base::timeout::suggested(
                boost::beast::role_type::client));
            ws.handshake(host, "/");
            
            // Subscribe
            json sub;
            sub["type"] = "subscribe";
            sub["channel"] = "level2";
            sub["product_ids"] = products_;
            
            std::string sub_msg = sub.dump();
            std::cout << "[COINBASE] Subscribing to " << products_.size() << " products\n";
            ws.write(boost::asio::buffer(sub_msg));
            
            // Read loop
            boost::beast::flat_buffer buffer;
            uint64_t message_count = 0;
            
            while (true) {
                try {
                    ws.read(buffer);
                    std::string raw = boost::beast::buffers_to_string(buffer.data());
                    
                    json msg = json::parse(raw);
                    
                    // Process Coinbase events
                    if (msg.contains("events") && msg["events"].is_array()) {
                        for (const auto& event : msg["events"]) {
                            std::string event_type = event.value("type", "unknown");
                            std::string product_id = event.value("product_id", "UNKNOWN");
                            
                            // Only process snapshots (full orderbook), not incremental updates
                            if (event_type == "snapshot" && event.contains("updates") && event["updates"].is_array()) {
                                // Reconstruct full orderbook from snapshot updates
                                json orderbook;
                                orderbook["product_id"] = product_id;
                                orderbook["bids"] = json::array();
                                orderbook["asks"] = json::array();
                                
                                for (const auto& update : event["updates"]) {
                                    std::string side = update.value("side", "");
                                    std::string price = update.value("price_level", "0");
                                    std::string size = update.value("new_quantity", "0");
                                    
                                    if (side == "bid") {
                                        orderbook["bids"].push_back({price, size});
                                    } else if (side == "offer") {
                                        orderbook["asks"].push_back({price, size});
                                    }
                                }
                                
                                // Only push if we have both bids and asks
                                if (!orderbook["bids"].empty() && !orderbook["asks"].empty()) {
                                    auto quote = pipeline::MultiExchangeNormalizer::normalize_coinbase(orderbook);
                                    queue_->try_enqueue(quote);
                                    message_count++;
                                    
                                    if (message_count % 10 == 0) {
                                        std::cout << "[COINBASE] Processed " << message_count << " snapshots\n";
                                    }
                                }
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[COINBASE] Parse error: " << e.what() << "\n";
                }
                
                buffer.consume(buffer.size());
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[COINBASE] Connection error: " << e.what() << "\n";
            throw;
        }
    }
    
private:
    std::vector<std::string> products_;
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> queue_;
};

// Binance adapter that pushes to queue
class BinanceConnector {
public:
    BinanceConnector(const std::vector<std::string>& symbols,
                     std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> queue)
        : symbols_(symbols), queue_(queue) {}
    
    void run() {
        try {
            std::cout << "[BINANCE] Connecting to " << symbols_.size() << " symbols...\n";
            
            // Create separate thread for each symbol to properly track them
            std::vector<std::thread> threads;
            for (size_t i = 0; i < symbols_.size(); ++i) {
                threads.emplace_back([this, i]() {
                    run_symbol(symbols_[i]);
                });
            }
            
            // Wait for all threads
            for (auto& t : threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[BINANCE] Connection error: " << e.what() << "\n";
            throw;
        }
    }
    
private:
    void run_symbol(const std::string& symbol) {
        try {
            std::vector<std::string> single_symbol = {symbol};
            BinanceWSClient client(single_symbol);
            client.connect();
            
            uint64_t message_count = 0;
            
            while (true) {
                std::string raw = client.read_message();
                
                try {
                    json msg = json::parse(raw);
                    
                    // Binance sends: 1) subscription confirmation, 2) depth snapshots
                    // Handle depth snapshots (orderbook snapshots)
                    if (msg.contains("lastUpdateId") && msg.contains("bids") && msg.contains("asks")) {
                        // This is a depth snapshot - we know which symbol because this thread handles only one
                        auto quote = pipeline::MultiExchangeNormalizer::normalize_binance(msg, symbol);
                        queue_->try_enqueue(quote);
                        message_count++;
                        
                        if (message_count % 50 == 0) {
                            std::cout << "[BINANCE-" << symbol << "] " << message_count << " snapshots\n";
                        }
                    }
                } catch (const std::exception& e) {
                    // Ignore parse errors silently
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[BINANCE-" << symbol << "] Error: " << e.what() << "\n";
        }
    }
    
    std::vector<std::string> symbols_;
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> queue_;
};

// Kraken adapter - TEMPORARILY DISABLED due to websocketpp compatibility
// TODO: Fix websocketpp compatibility with Boost 1.86+
/*
class KrakenConnector {
public:
    KrakenConnector(const std::vector<std::string>& symbols,
                    std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> queue)
        : symbols_(symbols), queue_(queue), message_count_(0) {}
    
    void run() {
        std::cout << "[KRAKEN] DISABLED - websocketpp compatibility issue\n";
    }
    
private:
    std::vector<std::string> symbols_;
    std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> queue_;
    std::atomic<uint64_t> message_count_;
};
*/

// Multi-Exchange Manager
class MultiExchangeConnector {
public:
    MultiExchangeConnector(std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> queue)
        : queue_(queue), running_(true) {}
    
    void add_coinbase(const std::vector<std::string>& products) {
        std::cout << "[MULTI-EX] Adding Coinbase with " << products.size() << " products\n";
        
        threads_.emplace_back([this, products]() {
            CoinbaseConnector connector(products, queue_);
            connector.run();
        });
    }
    
    void add_binance(const std::vector<std::string>& symbols) {
        std::cout << "[MULTI-EX] Adding Binance with " << symbols.size() << " symbols\n";
        
        threads_.emplace_back([this, symbols]() {
            BinanceConnector connector(symbols, queue_);
            connector.run();
        });
    }
    
    void add_kraken(const std::vector<std::string>& symbols) {
        std::cout << "[MULTI-EX] Kraken DISABLED - websocketpp compatibility issue\n";
        std::cout << "[MULTI-EX] TODO: Fix websocketpp with Boost 1.86+\n";
        // Will be re-enabled after fixing websocketpp compatibility
        /*
        std::cout << "[MULTI-EX] Adding Kraken with " << symbols.size() << " symbols\n";
        
        threads_.emplace_back([this, symbols]() {
            KrakenConnector connector(symbols, queue_);
            connector.run();
        });
        */
    }
    
    void wait() {
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    
    void stop() {
        running_.store(false);
    }
    
private:
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> queue_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_;
};

} // namespace exchanges
