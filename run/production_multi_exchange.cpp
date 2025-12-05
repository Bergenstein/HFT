// run/production_multi_exchange.cpp - PRODUCTION Step 5: Multi-Exchange HFT Pipeline
// 
// STEP 5 COMPLETE IMPLEMENTATION:
// Multiple Exchanges → Normalizer → Lock-Free MPMC Queue → Consumer Thread
//                                                         → In-Memory Cache (latest)
//                                                         → SQLite (historical)
//                                                         → ZMQ Pub/Sub (for strategies)
//
// Exchanges: Coinbase, Binance, Kraken, OKX, Bybit

#include "../md/ws_l2_client.hpp"
#include "../md/normalizer.hpp"
#include "../md/binance_ws_client.hpp"
#include "../md/kraken_ws_client.hpp"
#include "../md/okx_ws_client.hpp"
#include "../md/bybit_ws_client.hpp"
#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalized_data.hpp"
#include "../pipeline/normalizer.hpp"
#include "../storage/sqlite/market_data_store.hpp"
#include "../storage/inmem/latest_quotes.hpp"
#include "../zmq/market_data_server.hpp"
#include "../core/cpu_affinity.hpp"
#include <thread>
#include <atomic>
#include <iostream>
#include <csignal>
#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;
using namespace pipeline;
using namespace storage;

// Global state
std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_quotes_received{0};
std::atomic<uint64_t> g_quotes_cached{0};
std::atomic<uint64_t> g_quotes_persisted{0};
std::atomic<uint64_t> g_quotes_published{0};

// Lock-free MPMC queue (1M capacity)
MPMCQueue<NormalizedQuote> g_quote_queue(1048576);

// Storage layers
LatestQuotesCache g_inmem_cache;
MarketDataStore* g_sqlite_store = nullptr;
hft::MarketDataServer* g_zmq_publisher = nullptr;

void signal_handler(int) {
    std::cout << "\n[MAIN] Shutdown signal received\n";
    g_running = false;
}

// Consumer thread: Dequeues → Updates in-memory cache + SQLite + ZMQ
void quote_consumer_thread() {
    std::cout << "[CONSUMER] Started (Thread ID: " << std::this_thread::get_id() << ")\n";
    
    if (core::pin_thread_to_core(2)) {
        std::cout << "[CONSUMER] Pinned to CPU core 2\n";
    }
    
    NormalizedQuote quote;
    int batch_count = 0;
    const int BATCH_SIZE = 1000;
    
    g_sqlite_store->begin_transaction();
    
    while (g_running) {
        if (g_quote_queue.try_dequeue(quote)) {
            // Step 1: In-Memory Cache (latest quotes)
            g_inmem_cache.update(quote);
            g_quotes_cached++;
            
            // Step 2: SQLite (historical, batched)
            if (g_sqlite_store->insert_quote(quote)) {
                g_quotes_persisted++;
                batch_count++;
                
                if (batch_count >= BATCH_SIZE) {
                    g_sqlite_store->commit_transaction();
                    g_sqlite_store->begin_transaction();
                    batch_count = 0;
                }
            }
            
            // Step 3: ZMQ Publish (for strategies)
            std::vector<std::pair<double, double>> bids, asks;
            for (size_t i = 0; i < std::min(size_t(10), quote.bids.size()); ++i) {
                bids.push_back(quote.bids[i]);
            }
            for (size_t i = 0; i < std::min(size_t(10), quote.asks.size()); ++i) {
                asks.push_back(quote.asks[i]);
            }
            
            if (!bids.empty() || !asks.empty()) {
                g_zmq_publisher->publish_update(
                    quote.exchange + ":" + quote.product_id,
                    bids, asks, quote.sequence
                );
                g_quotes_published++;
            }
            
            g_quotes_received++;
        } else {
            std::this_thread::yield();
        }
    }
    
    g_sqlite_store->commit_transaction();
    std::cout << "[CONSUMER] Stopped\n";
}

// COINBASE Feed Thread
class CoinbaseFeedThread {
public:
    CoinbaseFeedThread(const std::vector<std::string>& products)
        : products_(products), running_(false) {}
    
    void start() {
        running_ = true;
        thread_ = std::thread([this]() {
            std::cout << "[COINBASE] Starting WebSocket feed...\n";
            
            try {
                WsL2Client client(products_);
                
                auto callback = [&](const std::string& raw_msg) {
                    if (!running_) return;
                    
                    try {
                        std::vector<L2Snapshot> snapshots;
                        std::vector<L2Update> updates;
                        Normalizer::parse(raw_msg, snapshots, updates);
                        
                        for (const auto& snap : snapshots) {
                            NormalizedQuote nq;
                            nq.exchange = "coinbase";
                            nq.product_id = snap.product;
                            nq.sequence = snap.seq;
                            nq.bids = snap.bids;
                            nq.asks = snap.asks;
                            if (!snap.bids.empty()) {
                                nq.best_bid = snap.bids[0].first;
                                nq.bid_size = snap.bids[0].second;
                            }
                            if (!snap.asks.empty()) {
                                nq.best_ask = snap.asks[0].first;
                                nq.ask_size = snap.asks[0].second;
                            }
                            nq.exchange_timestamp = std::chrono::system_clock::now();
                            nq.local_timestamp = std::chrono::system_clock::now();
                            
                            while (!g_quote_queue.try_enqueue(nq) && running_) {
                                std::this_thread::yield();
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[COINBASE] Parse error: " << e.what() << "\n";
                    }
                };
                
                client.run([&](const std::string& msg) { callback(msg); });
                
            } catch (const std::exception& e) {
                std::cerr << "[COINBASE] Error: " << e.what() << "\n";
            }
            
            std::cout << "[COINBASE] Feed stopped\n";
        });
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    
private:
    std::vector<std::string> products_;
    std::atomic<bool> running_;
    std::thread thread_;
};

// BINANCE Feed Thread
class BinanceFeedThread {
public:
    BinanceFeedThread(const std::vector<std::string>& symbols)
        : symbols_(symbols), running_(false) {}
    
    void start() {
        running_ = true;
        thread_ = std::thread([this]() {
            std::cout << "[BINANCE] Starting WebSocket feed...\n";
            
            try {
                BinanceWSClient client("Binance");
                
                client.set_message_callback([&](const std::string& raw_msg) {
                    if (!running_) return;
                    
                    try {
                        json msg = json::parse(raw_msg);
                        
                        for (const auto& symbol : symbols_) {
                            NormalizedQuote nq = MultiExchangeNormalizer::normalize_binance(msg, symbol);
                            
                            while (!g_quote_queue.try_enqueue(nq) && running_) {
                                std::this_thread::yield();
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[BINANCE] Parse error: " << e.what() << "\n";
                    }
                });
                
                client.subscribe_orderbook(symbols_);
                
            } catch (const std::exception& e) {
                std::cerr << "[BINANCE] Error: " << e.what() << "\n";
            }
            
            std::cout << "[BINANCE] Feed stopped\n";
        });
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    
private:
    std::vector<std::string> symbols_;
    std::atomic<bool> running_;
    std::thread thread_;
};

// KRAKEN Feed Thread  
class KrakenFeedThread {
public:
    KrakenFeedThread(const std::vector<std::string>& symbols)
        : symbols_(symbols), running_(false) {}
    
    void start() {
        running_ = true;
        thread_ = std::thread([this]() {
            std::cout << "[KRAKEN] Starting WebSocket feed...\n";
            
            try {
                KrakenWSClient client("Kraken");
                
                client.set_message_callback([&](const std::string& raw_msg) {
                    if (!running_) return;
                    
                    try {
                        json msg = json::parse(raw_msg);
                        
                        for (const auto& symbol : symbols_) {
                            NormalizedQuote nq = MultiExchangeNormalizer::normalize_kraken(msg, symbol);
                            
                            while (!g_quote_queue.try_enqueue(nq) && running_) {
                                std::this_thread::yield();
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[KRAKEN] Parse error: " << e.what() << "\n";
                    }
                });
                
                client.subscribe_orderbook(symbols_);
                
            } catch (const std::exception& e) {
                std::cerr << "[KRAKEN] Error: " << e.what() << "\n";
            }
            
            std::cout << "[KRAKEN] Feed stopped\n";
        });
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    
private:
    std::vector<std::string> symbols_;
    std::atomic<bool> running_;
    std::thread thread_;
};

// OKX Feed Thread
class OKXFeedThread {
public:
    OKXFeedThread(const std::vector<std::string>& symbols)
        : symbols_(symbols), running_(false) {}
    
    void start() {
        running_ = true;
        thread_ = std::thread([this]() {
            std::cout << "[OKX] Starting WebSocket feed...\n";
            
            try {
                OKXWSClient client("OKX");
                
                client.set_message_callback([&](const std::string& raw_msg) {
                    if (!running_) return;
                    
                    try {
                        json msg = json::parse(raw_msg);
                        
                        for (const auto& symbol : symbols_) {
                            NormalizedQuote nq = MultiExchangeNormalizer::normalize_okx(msg, symbol);
                            
                            while (!g_quote_queue.try_enqueue(nq) && running_) {
                                std::this_thread::yield();
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[OKX] Parse error: " << e.what() << "\n";
                    }
                });
                
                client.subscribe_orderbook(symbols_);
                
            } catch (const std::exception& e) {
                std::cerr << "[OKX] Error: " << e.what() << "\n";
            }
            
            std::cout << "[OKX] Feed stopped\n";
        });
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    
private:
    std::vector<std::string> symbols_;
    std::atomic<bool> running_;
    std::thread thread_;
};

// BYBIT Feed Thread
class BybitFeedThread {
public:
    BybitFeedThread(const std::vector<std::string>& symbols)
        : symbols_(symbols), running_(false) {}
    
    void start() {
        running_ = true;
        thread_ = std::thread([this]() {
            std::cout << "[BYBIT] Starting WebSocket feed...\n";
            
            try {
                BybitWSClient client("Bybit");
                
                client.set_message_callback([&](const std::string& raw_msg) {
                    if (!running_) return;
                    
                    try {
                        json msg = json::parse(raw_msg);
                        
                        for (const auto& symbol : symbols_) {
                            NormalizedQuote nq = MultiExchangeNormalizer::normalize_bybit(msg, symbol);
                            
                            while (!g_quote_queue.try_enqueue(nq) && running_) {
                                std::this_thread::yield();
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[BYBIT] Parse error: " << e.what() << "\n";
                    }
                });
                
                client.subscribe_orderbook(symbols_);
                
            } catch (const std::exception& e) {
                std::cerr << "[BYBIT] Error: " << e.what() << "\n";
            }
            
            std::cout << "[BYBIT] Feed stopped\n";
        });
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    
private:
    std::vector<std::string> symbols_;
    std::atomic<bool> running_;
    std::thread thread_;
};

// Monitoring thread
void monitoring_thread() {
    std::cout << "[MONITOR] Started\n";
    
    auto start_time = std::chrono::steady_clock::now();
    int iteration = 0;
    
    while (g_running && iteration < 60) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
        
        auto qps = elapsed > 0 ? g_quotes_received.load() / elapsed : 0;
        
        std::cout << "\n[STATS] T+" << elapsed << "s"
                  << " | RX: " << g_quotes_received.load()
                  << " | QPS: " << qps
                  << " | Cache: " << g_quotes_cached.load()
                  << " | DB: " << g_quotes_persisted.load()
                  << " | ZMQ: " << g_quotes_published.load()
                  << "\n";
        
        // Show latest quotes from cache
        NormalizedQuote latest;
        if (g_inmem_cache.get("coinbase", "BTC-USD", latest)) {
            std::cout << "  [coinbase] BTC-USD: $" << latest.best_bid << "/" << latest.best_ask
                      << " spread=" << latest.spread_bps() << "bps\n";
        }
        if (g_inmem_cache.get("binance", "BTC-USDT", latest)) {
            std::cout << "  [binance] BTC-USDT: $" << latest.best_bid << "/" << latest.best_ask << "\n";
        }
        
        iteration++;
    }
    
    std::cout << "\n[MONITOR] Stopping system...\n";
    g_running = false;
}

int main() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════════╗
║   PRODUCTION MULTI-EXCHANGE HFT PIPELINE - STEP 5 COMPLETE       ║
║                                                                   ║
║  Exchanges: Coinbase, Binance, Kraken, OKX, Bybit                ║
║  Pipeline:  WebSocket → Normalize → MPMC Queue → Consumer        ║
║  Storage:   In-Memory Cache + SQLite (WAL) + ZMQ Pub/Sub         ║
║  Features:  Lock-Free Queues, CPU Pinning, Batched Writes        ║
╚═══════════════════════════════════════════════════════════════════╝
)" << "\n";
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize ZMQ publisher
    std::cout << "[MAIN] Initializing ZMQ publisher on tcp://*:5555...\n";
    g_zmq_publisher = new hft::MarketDataServer("tcp://*:5555");
    
    // Initialize SQLite
    std::cout << "[MAIN] Initializing SQLite storage (WAL mode)...\n";
    g_sqlite_store = new MarketDataStore("production_market_data.db");
    std::cout << "[MAIN] Current DB rows: " << g_sqlite_store->get_quote_count() << "\n";
    
    // Start consumer thread
    std::cout << "[MAIN] Starting consumer thread...\n";
    std::thread consumer(quote_consumer_thread);
    
    // Start monitoring
    std::cout << "[MAIN] Starting monitoring thread...\n";
    std::thread monitor(monitoring_thread);
    
    // Start exchange feeds
    std::cout << "[MAIN] Starting exchange feeds...\n";
    CoinbaseFeedThread coinbase_feed({"BTC-USD", "ETH-USD"});
    BinanceFeedThread binance_feed({"BTCUSDT", "ETHUSDT"});
    KrakenFeedThread kraken_feed({"XBT/USD", "ETH/USD"});
    OKXFeedThread okx_feed({"BTC-USDT", "ETH-USDT"});
    BybitFeedThread bybit_feed({"BTCUSDT", "ETHUSDT"});
    
    coinbase_feed.start();
    binance_feed.start();
    kraken_feed.start();
    okx_feed.start();
    bybit_feed.start();
    
    // Wait for monitoring
    monitor.join();
    
    // Shutdown
    std::cout << "[MAIN] Stopping all feeds...\n";
    coinbase_feed.stop();
    binance_feed.stop();
    kraken_feed.stop();
    okx_feed.stop();
    bybit_feed.stop();
    
    std::cout << "[MAIN] Stopping consumer...\n";
    consumer.join();
    
    // Final statistics
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "FINAL STATISTICS\n";
    std::cout << std::string(70, '=') << "\n";
    std::cout << "Quotes received:     " << g_quotes_received.load() << "\n";
    std::cout << "Cached (in-memory):  " << g_quotes_cached.load() << "\n";
    std::cout << "Persisted (SQLite):  " << g_quotes_persisted.load() << "\n";
    std::cout << "Published (ZMQ):     " << g_quotes_published.load() << "\n";
    std::cout << "Cache size:          " << g_inmem_cache.size() << "\n";
    std::cout << "DB total rows:       " << g_sqlite_store->get_quote_count() << "\n";
    std::cout << "\n✓ Step 5 COMPLETE: Multi-exchange pipeline operational\n\n";
    
    delete g_zmq_publisher;
    delete g_sqlite_store;
    return 0;
}
