// run/test_multi_exchange_hot_cold.cpp
//==============================================================================
// MULTI-EXCHANGE HOT PATH + COLD PATH INTEGRATION TEST
//==============================================================================
// Tests both Coinbase AND Binance (and other exchanges) in the hot path.
// This addresses the issue of "why is hot path only Coinbase?"
//
// Architecture:
//   [Coinbase WS] ---\
//                     \---> [MPMC Queue] ---> [Cold Path] ---> [SQLite]
//   [Binance WS]  ----/                                   \
//                                                          \---> [ZMQ Pub]
//   [Coinbase WS] ---> [SPSC Queue] ---> [Hot Path Coinbase] ---> [Signals]
//   [Binance WS]  ---> [SPSC Queue] ---> [Hot Path Binance]  ---> [Signals]
//
//==============================================================================

#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <memory>
#include <random>
#include <chrono>
#include <map>
#include <iomanip>
#include <limits>

#include "../pipeline/spsc_queue.hpp"
#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalized_data.hpp"

std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\n[SIGNAL] Caught signal " << sig << ", shutting down...\n";
    g_running.store(false);
}

//==============================================================================
// MOCK MARKET DATA GENERATOR (simulates exchange WebSocket)
//==============================================================================
class MockExchangeFeed {
public:
    MockExchangeFeed(
        const std::string& exchange,
        const std::vector<std::string>& products,
        std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue,
        std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> spsc_queue,
        double base_price = 50000.0
    )
        : exchange_(exchange),
          products_(products),
          mpmc_queue_(mpmc_queue),
          spsc_queue_(spsc_queue),
          base_price_(base_price),
          running_(false),
          rng_(std::random_device{}()),
          price_noise_(0.0, 0.001),
          size_dist_(0.1, 10.0)
    {}
    
    void start() {
        running_ = true;
        thread_ = std::thread(&MockExchangeFeed::run, this);
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    
    uint64_t get_messages_sent() const { return messages_sent_; }
    
private:
    void run() {
        uint64_t sequence = 0;
        double price = base_price_;
        
        // Price walk parameters per product
        std::map<std::string, double> prices;
        for (const auto& product : products_) {
            if (product.find("BTC") != std::string::npos) {
                prices[product] = 97000.0;
            } else if (product.find("ETH") != std::string::npos) {
                prices[product] = 3400.0;
            } else if (product.find("SOL") != std::string::npos) {
                prices[product] = 240.0;
            } else {
                prices[product] = 100.0;
            }
        }
        
        while (running_) {
            for (const auto& product : products_) {
                // Random walk price
                double& p = prices[product];
                p *= (1.0 + price_noise_(rng_));
                
                // Add exchange-specific spread (Binance typically tighter)
                double spread = (exchange_ == "binance") ? 0.5 : 1.0;
                if (product.find("BTC") == std::string::npos) {
                    spread *= 0.01; // Smaller spread for cheaper assets
                }
                
                pipeline::NormalizedQuote quote;
                quote.exchange = exchange_;
                quote.product_id = product;
                
                // Parse base/quote from product
                auto dash = product.find('-');
                if (dash != std::string::npos) {
                    quote.base = product.substr(0, dash);
                    quote.quote = product.substr(dash + 1);
                } else {
                    // Binance format: BTCUSDT
                    if (product.find("USDT") != std::string::npos) {
                        quote.base = product.substr(0, product.find("USDT"));
                        quote.quote = "USDT";
                    }
                }
                
                quote.sequence = sequence++;
                quote.local_timestamp = std::chrono::system_clock::now();
                quote.exchange_timestamp = quote.local_timestamp;
                
                quote.best_bid = p - spread / 2.0;
                quote.best_ask = p + spread / 2.0;
                quote.bid_size = size_dist_(rng_);
                quote.ask_size = size_dist_(rng_);
                
                // Add depth
                for (int i = 0; i < 5; i++) {
                    quote.bids.push_back({quote.best_bid - i * spread * 0.5, size_dist_(rng_)});
                    quote.asks.push_back({quote.best_ask + i * spread * 0.5, size_dist_(rng_)});
                }
                
                // Push to both queues
                mpmc_queue_->try_enqueue(quote);
                spsc_queue_->try_push(quote);
                messages_sent_++;
            }
            
            // Simulate ~100 updates per second per product
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::string exchange_;
    std::vector<std::string> products_;
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue_;
    std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> spsc_queue_;
    double base_price_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::mt19937 rng_;
    std::normal_distribution<double> price_noise_;
    std::uniform_real_distribution<double> size_dist_;
    std::atomic<uint64_t> messages_sent_{0};
};

//==============================================================================
// HOT PATH PROCESSOR (per-exchange, latency-critical)
//==============================================================================
class HotPathProcessor {
public:
    HotPathProcessor(
        const std::string& exchange,
        std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> queue
    )
        : exchange_(exchange), queue_(queue), running_(false) {}
    
    void start() {
        running_ = true;
        thread_ = std::thread(&HotPathProcessor::run, this);
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    
    uint64_t get_messages_processed() const { return messages_processed_; }
    uint64_t get_signals_generated() const { return signals_generated_; }
    
private:
    void run() {
        // Track last price per product for signal generation
        std::map<std::string, double> last_prices;
        std::map<std::string, double> last_imbalances;
        
        while (running_) {
            pipeline::NormalizedQuote quote;
            
            while (queue_->try_pop(quote)) {
                messages_processed_++;
                
                double mid = (quote.best_bid + quote.best_ask) / 2.0;
                double imbalance = 0.0;
                if (quote.bid_size + quote.ask_size > 0) {
                    imbalance = (quote.bid_size - quote.ask_size) / (quote.bid_size + quote.ask_size);
                }
                
                // Simple momentum + imbalance signal
                auto it = last_prices.find(quote.product_id);
                if (it != last_prices.end()) {
                    double price_change = (mid - it->second) / it->second;
                    double prev_imb = last_imbalances[quote.product_id];
                    
                    // Signal: strong imbalance + price momentum in same direction
                    if (std::abs(imbalance) > 0.3 && std::abs(price_change) > 0.0001) {
                        if ((imbalance > 0 && price_change > 0) || 
                            (imbalance < 0 && price_change < 0)) {
                            signals_generated_++;
                            
                            // In production, this would trigger order execution
                            // For now, just count signals
                        }
                    }
                }
                
                last_prices[quote.product_id] = mid;
                last_imbalances[quote.product_id] = imbalance;
            }
            
            // Small sleep to prevent busy-waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    std::string exchange_;
    std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> queue_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::atomic<uint64_t> messages_processed_{0};
    std::atomic<uint64_t> signals_generated_{0};
};

//==============================================================================
// COLD PATH AGGREGATOR (multi-exchange, storage + analytics)
//==============================================================================
class ColdPathAggregator {
public:
    ColdPathAggregator(
        std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> queue,
        const std::string& db_path = "multi_exchange_test.db"
    )
        : queue_(queue), db_path_(db_path), running_(false) {}
    
    void start() {
        running_ = true;
        thread_ = std::thread(&ColdPathAggregator::run, this);
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    
    uint64_t get_messages_processed() const { return messages_processed_; }
    uint64_t get_arb_opportunities() const { return arb_opportunities_; }
    
private:
    void run() {
        // Track latest quotes per exchange/product for arb detection
        std::map<std::string, std::map<std::string, pipeline::NormalizedQuote>> latest_quotes;
        
        while (running_) {
            pipeline::NormalizedQuote quote;
            
            while (queue_->try_dequeue(quote)) {
                messages_processed_++;
                
                // Store latest quote
                latest_quotes[quote.product_id][quote.exchange] = quote;
                
                // Check for cross-exchange arbitrage
                const auto& product_quotes = latest_quotes[quote.product_id];
                if (product_quotes.size() >= 2) {
                    // Find best bid and best ask across exchanges
                    double best_bid = 0.0;
                    double best_ask = std::numeric_limits<double>::max();
                    std::string best_bid_exchange, best_ask_exchange;
                    
                    for (const auto& [ex, q] : product_quotes) {
                        if (q.best_bid > best_bid) {
                            best_bid = q.best_bid;
                            best_bid_exchange = ex;
                        }
                        if (q.best_ask < best_ask) {
                            best_ask = q.best_ask;
                            best_ask_exchange = ex;
                        }
                    }
                    
                    // Check for crossed market (arbitrage opportunity)
                    if (best_bid > best_ask && best_bid_exchange != best_ask_exchange) {
                        arb_opportunities_++;
                        // In production, would trigger arb execution
                    }
                }
            }
            
            // Batch write to SQLite would go here
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> queue_;
    std::string db_path_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::atomic<uint64_t> messages_processed_{0};
    std::atomic<uint64_t> arb_opportunities_{0};
};

//==============================================================================
// MAIN
//==============================================================================
int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::cout << R"(
╔══════════════════════════════════════════════════════════════════════════════╗
║            MULTI-EXCHANGE HOT PATH + COLD PATH INTEGRATION TEST              ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  Exchanges: Coinbase, Binance                                                ║
║  Products: BTC-USD, ETH-USD, SOL-USD (Coinbase)                              ║
║            BTCUSDT, ETHUSDT, SOLUSDT (Binance)                               ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  Architecture:                                                               ║
║    [Exchange] → [SPSC Queue] → [Hot Path] → [Signals]                        ║
║    [Exchange] → [MPMC Queue] → [Cold Path] → [Arb Detection]                 ║
╚══════════════════════════════════════════════════════════════════════════════╝
)" << "\n";
    
    // Create shared queues
    auto mpmc_queue = std::make_shared<pipeline::MPMCQueue<pipeline::NormalizedQuote>>(1048576);
    auto coinbase_spsc = std::make_shared<pipeline::SPSCQueue<pipeline::NormalizedQuote>>(524288);
    auto binance_spsc = std::make_shared<pipeline::SPSCQueue<pipeline::NormalizedQuote>>(524288);
    
    std::cout << "[INIT] ✓ Queues created\n";
    std::cout << "       - MPMC Queue (cold path): 1M capacity\n";
    std::cout << "       - SPSC Queue (Coinbase hot): 512K capacity\n";
    std::cout << "       - SPSC Queue (Binance hot): 512K capacity\n\n";
    
    // Create exchange feeds
    std::vector<std::string> coinbase_products = {"BTC-USD", "ETH-USD", "SOL-USD"};
    std::vector<std::string> binance_products = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    
    MockExchangeFeed coinbase_feed("coinbase", coinbase_products, mpmc_queue, coinbase_spsc);
    MockExchangeFeed binance_feed("binance", binance_products, mpmc_queue, binance_spsc);
    
    // Create hot path processors (one per exchange for minimum latency)
    HotPathProcessor coinbase_hot("coinbase", coinbase_spsc);
    HotPathProcessor binance_hot("binance", binance_spsc);
    
    // Create cold path aggregator (handles all exchanges)
    ColdPathAggregator cold_path(mpmc_queue);
    
    // Start all components
    std::cout << "[START] Starting components...\n";
    
    cold_path.start();
    std::cout << "       ✓ Cold path aggregator started\n";
    
    coinbase_hot.start();
    std::cout << "       ✓ Coinbase hot path started\n";
    
    binance_hot.start();
    std::cout << "       ✓ Binance hot path started\n";
    
    coinbase_feed.start();
    std::cout << "       ✓ Coinbase feed started (" << coinbase_products.size() << " products)\n";
    
    binance_feed.start();
    std::cout << "       ✓ Binance feed started (" << binance_products.size() << " products)\n";
    
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  TEST RUNNING - Press Ctrl+C to stop\n";
    std::cout << "════════════════════════════════════════════════════════════════════════════════\n\n";
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Monitor loop
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        // Calculate rates
        double coinbase_rate = static_cast<double>(coinbase_hot.get_messages_processed()) / uptime;
        double binance_rate = static_cast<double>(binance_hot.get_messages_processed()) / uptime;
        double cold_rate = static_cast<double>(cold_path.get_messages_processed()) / uptime;
        
        std::cout << "\n┌──────────────────────────────────────────────────────────────────────────────┐\n";
        std::cout << "│ STATS (Uptime: " << uptime << " seconds)" << std::string(60 - std::to_string(uptime).length(), ' ') << "│\n";
        std::cout << "├──────────────────────────────────────────────────────────────────────────────┤\n";
        
        std::cout << "│ [FEEDS]                                                                      │\n";
        std::cout << "│   Coinbase: " << std::setw(10) << coinbase_feed.get_messages_sent() 
                  << " msgs sent (" << std::fixed << std::setprecision(1) << coinbase_rate << " msgs/sec)"
                  << std::string(30, ' ') << "│\n";
        std::cout << "│   Binance:  " << std::setw(10) << binance_feed.get_messages_sent() 
                  << " msgs sent (" << binance_rate << " msgs/sec)"
                  << std::string(30, ' ') << "│\n";
        
        std::cout << "├──────────────────────────────────────────────────────────────────────────────┤\n";
        
        std::cout << "│ [HOT PATH]                                                                   │\n";
        std::cout << "│   Coinbase: " << std::setw(10) << coinbase_hot.get_messages_processed() 
                  << " processed, " << std::setw(6) << coinbase_hot.get_signals_generated() << " signals"
                  << std::string(24, ' ') << "│\n";
        std::cout << "│   Binance:  " << std::setw(10) << binance_hot.get_messages_processed() 
                  << " processed, " << std::setw(6) << binance_hot.get_signals_generated() << " signals"
                  << std::string(24, ' ') << "│\n";
        
        std::cout << "├──────────────────────────────────────────────────────────────────────────────┤\n";
        
        std::cout << "│ [COLD PATH]                                                                  │\n";
        std::cout << "│   Total:    " << std::setw(10) << cold_path.get_messages_processed() 
                  << " processed (" << cold_rate << " msgs/sec)"
                  << std::string(30, ' ') << "│\n";
        std::cout << "│   Arb Opps: " << std::setw(10) << cold_path.get_arb_opportunities() 
                  << " detected" << std::string(40, ' ') << "│\n";
        
        std::cout << "└──────────────────────────────────────────────────────────────────────────────┘\n";
    }
    
    // Shutdown
    std::cout << "\n[SHUTDOWN] Stopping components...\n";
    
    coinbase_feed.stop();
    binance_feed.stop();
    std::cout << "          ✓ Feeds stopped\n";
    
    coinbase_hot.stop();
    binance_hot.stop();
    std::cout << "          ✓ Hot paths stopped\n";
    
    cold_path.stop();
    std::cout << "          ✓ Cold path stopped\n";
    
    // Final stats
    auto total_uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                            FINAL RESULTS                                     ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Total uptime: " << total_uptime << " seconds" 
              << std::string(60 - std::to_string(total_uptime).length(), ' ') << "║\n";
    std::cout << "║ Coinbase messages: " << coinbase_hot.get_messages_processed() 
              << std::string(55 - std::to_string(coinbase_hot.get_messages_processed()).length(), ' ') << "║\n";
    std::cout << "║ Binance messages:  " << binance_hot.get_messages_processed()
              << std::string(55 - std::to_string(binance_hot.get_messages_processed()).length(), ' ') << "║\n";
    std::cout << "║ Total signals: " << (coinbase_hot.get_signals_generated() + binance_hot.get_signals_generated())
              << std::string(59 - std::to_string(coinbase_hot.get_signals_generated() + binance_hot.get_signals_generated()).length(), ' ') << "║\n";
    std::cout << "║ Arb opportunities: " << cold_path.get_arb_opportunities()
              << std::string(55 - std::to_string(cold_path.get_arb_opportunities()).length(), ' ') << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    
    return 0;
}
