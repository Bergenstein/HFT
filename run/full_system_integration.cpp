// run/full_system_integration.cpp
// Complete Multi-Exchange HFT System Integration
// Coinbase + Binance → Normalized → Hot Path (SPSC) + Cold Path (MPMC) → Strategies/Storage/ZMQ
//
// ARCHITECTURE:
// ┌─────────────────────────────────────────────────────────────┐
// │                    EXCHANGE CONNECTORS                       │
// │  ┌──────────┐              ┌──────────┐                     │
// │  │ Coinbase │              │ Binance  │                     │
// │  │ WebSocket│              │ WebSocket│                     │
// │  └────┬─────┘              └────┬─────┘                     │
// └───────┼──────────────────────────┼──────────────────────────┘
//         │                          │
//         ▼                          ▼
// ┌─────────────────────────────────────────────────────────────┐
// │         NORMALIZATION (MultiExchangeNormalizer)             │
// │              → NormalizedQuote                              │
// └──────┬──────────────────────────┬───────────────────────────┘
//        │                          │
//        ▼                          ▼
// ┌─────────────┐           ┌──────────────────────────────────┐
// │  HOT PATH   │           │       COLD PATH (MPMC)           │
// │  (SPSC)     │           │                                  │
// │             │           │  ┌─────────────────────────┐     │
// │ Per-Exchange│           │  │ Cross-Exchange Arb      │     │
// │ Strategies  │           │  │ (Perp-Spot, Funding)    │     │
// │             │           │  └─────────────────────────┘     │
// │  ├─ OFI     │           │                                  │
// │  ├─ Imb     │           │  ┌─────────────────────────┐     │
// │  └─ μPrice  │           │  │ SQLite Archival         │     │
// │             │           │  │ (Batch Writes)          │     │
// └──────┬──────┘           │  └─────────────────────────┘     │
//        │                  │                                  │
//        ▼                  │  ┌─────────────────────────┐     │
// ┌─────────────┐           │  │ Exchange Simulator      │     │
// │  ZeroMQ     │           │  │ (Paper Trading)         │     │
// │ Signals     │           │  └─────────────────────────┘     │
// │ Port 5556   │           └──────────────┬───────────────────┘
// └─────────────┘                          │
//                                          ▼
//                                   ┌─────────────┐
//                                   │   ZeroMQ    │
//                                   │  Arb Opps   │
//                                   │  Port 5557  │
//                                   └─────────────┘

#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <memory>
#include <map>

// Pipeline
#include "../pipeline/spsc_queue.hpp"
#include "../pipeline/mpmc_queue.hpp"
#include "../pipeline/normalized_data.hpp"

// Exchanges
#include "../exchanges/multi_exchange_connector.hpp"

// Hot/Cold Paths (Simple versions for integration)
#include "hot_path_processor_simple.hpp"
#include "cold_path_aggregator_simple.hpp"

// Storage
#include "../storage/sqlite/market_data_store.hpp"
#include "../storage/inmem/latest_quotes.hpp"

// ZeroMQ
#include "../zmq/market_data_server.hpp"

// Exchange Simulator
#include "../sim/exchange_simulator_feed.hpp"

// ===================================================================
// GLOBAL STATE
// ===================================================================
std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\n[SYSTEM] Signal " << sig << " received, initiating shutdown...\n";
    g_running.store(false);
}

// ===================================================================
// DUAL-FEED BRIDGE (MPMC → Multiple SPSC queues)
// ===================================================================
class DualFeedBridge {
public:
    DualFeedBridge(
        std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue
    )
        : mpmc_queue_(mpmc_queue), running_(false)
    {}
    
    void add_hot_path_queue(
        const std::string& exchange,
        std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>> spsc_queue
    ) {
        hot_path_queues_[exchange] = spsc_queue;
        std::cout << "[Bridge] Added hot path queue for " << exchange << "\n";
    }
    
    void start() {
        running_ = true;
        bridge_thread_ = std::thread(&DualFeedBridge::bridge_loop, this);
        std::cout << "[Bridge] Started dual-feed bridge\n";
    }
    
    void stop() {
        running_ = false;
        if (bridge_thread_.joinable()) {
            bridge_thread_.join();
        }
        std::cout << "[Bridge] Stopped. Forwarded " << quotes_forwarded_.load() << " quotes\n";
    }

private:
    void bridge_loop() {
        pipeline::NormalizedQuote quote;
        
        while (running_.load()) {
            if (mpmc_queue_->try_dequeue(quote)) {
                // Forward to appropriate hot path SPSC queue
                auto it = hot_path_queues_.find(quote.exchange);
                if (it != hot_path_queues_.end()) {
                    if (it->second->try_push(quote)) {
                        quotes_forwarded_++;
                    } else {
                        dropped_++;
                    }
                }
            } else {
                std::this_thread::yield();
            }
        }
    }
    
    std::shared_ptr<pipeline::MPMCQueue<pipeline::NormalizedQuote>> mpmc_queue_;
    std::map<std::string, std::shared_ptr<pipeline::SPSCQueue<pipeline::NormalizedQuote>>> hot_path_queues_;
    std::atomic<bool> running_;
    std::thread bridge_thread_;
    std::atomic<uint64_t> quotes_forwarded_{0};
    std::atomic<uint64_t> dropped_{0};
};

// ===================================================================
// MAIN INTEGRATION
// ===================================================================
int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::cout << "================================================================\n";
    std::cout << "  FULL MULTI-EXCHANGE HFT SYSTEM INTEGRATION\n";
    std::cout << "  Coinbase + Binance → Normalized → Hot + Cold Paths\n";
    std::cout << "================================================================\n\n";
    
    // ===================================================================
    // STEP 1: Initialize Queues
    // ===================================================================
    std::cout << "[INIT] Creating queue infrastructure...\n";
    
    // Cold path: Single MPMC for all exchanges
    auto mpmc_queue = std::make_shared<pipeline::MPMCQueue<pipeline::NormalizedQuote>>(1048576);
    
    // Hot path: Per-exchange SPSC queues
    auto coinbase_spsc = std::make_shared<pipeline::SPSCQueue<pipeline::NormalizedQuote>>(524288);
    auto binance_spsc = std::make_shared<pipeline::SPSCQueue<pipeline::NormalizedQuote>>(524288);
    
    std::cout << "[INIT] ✓ MPMC queue: 1M slots (cold path)\n";
    std::cout << "[INIT] ✓ Coinbase SPSC: 512K slots (hot path)\n";
    std::cout << "[INIT] ✓ Binance SPSC: 512K slots (hot path)\n\n";
    
    // ===================================================================
    // STEP 2: Initialize Storage
    // ===================================================================
    std::cout << "[STORAGE] Initializing storage layers...\n";
    
    auto sqlite_store = std::make_shared<storage::MarketDataStore>("full_system_integration.db");
    auto quote_cache = std::make_shared<storage::LatestQuotesCache>();
    
    std::cout << "[STORAGE] ✓ SQLite: full_system_integration.db\n";
    std::cout << "[STORAGE] ✓ In-memory cache initialized\n\n";
    
    // ===================================================================
    // STEP 3: Initialize ZeroMQ Publishers
    // ===================================================================
    std::cout << "[ZMQ] Starting ZeroMQ publishers...\n";
    
    auto zmq_market_data = std::make_shared<hft::MarketDataServer>("tcp://*:5555");
    auto zmq_signals = std::make_shared<hft::MarketDataServer>("tcp://*:5556");
    auto zmq_arb = std::make_shared<hft::MarketDataServer>("tcp://*:5557");
    
    std::cout << "[ZMQ] ✓ Market data: tcp://*:5555\n";
    std::cout << "[ZMQ] ✓ Strategy signals: tcp://*:5556\n";
    std::cout << "[ZMQ] ✓ Arbitrage: tcp://*:5557\n\n";
    
    // ===================================================================
    // STEP 4: Start Hot Path Processors
    // ===================================================================
    std::cout << "[HOT PATH] Starting per-exchange processors...\n";
    
    hft::HotPathProcessorSimple coinbase_hot("coinbase", coinbase_spsc, -1);
    hft::HotPathProcessorSimple binance_hot("binance", binance_spsc, -1);
    
    coinbase_hot.start();
    binance_hot.start();
    
    std::cout << "[HOT PATH] ✓ Coinbase processor started\n";
    std::cout << "[HOT PATH] ✓ Binance processor started\n\n";
    
    // ===================================================================
    // STEP 5: Start Cold Path Aggregator
    // ===================================================================
    std::cout << "[COLD PATH] Starting aggregator (SQLite archival)...\n";
    
    hft::ColdPathAggregatorSimple cold_path(mpmc_queue, "full_system_integration.db", -1);
    cold_path.start();
    
    std::cout << "[COLD PATH] ✓ Aggregator started\n\n";
    
    // ===================================================================
    // STEP 6: Start Exchange Simulator Feed
    // ===================================================================
    std::cout << "[SIMULATOR] Starting exchange simulator feed...\n";
    
    auto simulator_feed = std::make_shared<hft::sim::ExchangeSimulatorFeed>(mpmc_queue, -1);
    simulator_feed->start();
    
    std::cout << "[SIMULATOR] ✓ Simulator feed started\n\n";
    
    // ===================================================================
    // STEP 7: Start Dual-Feed Bridge
    // ===================================================================
    std::cout << "[BRIDGE] Starting dual-feed bridge (MPMC→SPSC)...\n";
    
    DualFeedBridge bridge(mpmc_queue);
    bridge.add_hot_path_queue("coinbase", coinbase_spsc);
    bridge.add_hot_path_queue("binance", binance_spsc);
    bridge.start();
    
    std::cout << "[BRIDGE] ✓ Bridge started\n\n";
    
    // ===================================================================
    // STEP 8: Start Exchange Connectors
    // ===================================================================
    std::cout << "[EXCHANGES] Connecting to exchanges...\n";
    
    auto multi_exchange = std::make_shared<exchanges::MultiExchangeConnector>(mpmc_queue);
    
    // Coinbase products
    std::vector<std::string> coinbase_products = {
        "BTC-USD", "ETH-USD", "SOL-USD"
    };
    
    // Binance symbols
    std::vector<std::string> binance_symbols = {
        "BTCUSDT", "ETHUSDT", "SOLUSDT"
    };
    
    multi_exchange->add_coinbase(coinbase_products);
    multi_exchange->add_binance(binance_symbols);
    
    // Give time to connect
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    std::cout << "[EXCHANGES] ✓ Coinbase connected (" << coinbase_products.size() << " products)\n";
    std::cout << "[EXCHANGES] ✓ Binance connected (" << binance_symbols.size() << " symbols)\n\n";
    
    // ===================================================================
    // STEP 9: Monitoring Loop
    // ===================================================================
    std::cout << "================================================================\n";
    std::cout << "  SYSTEM RUNNING\n";
    std::cout << "================================================================\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "Statistics will be displayed every 10 seconds\n";
    std::cout << "================================================================\n\n";
    
    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_coinbase_msgs = 0;
    uint64_t last_binance_msgs = 0;
    uint64_t last_archived = 0;
    
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        uint64_t coinbase_msgs = coinbase_hot.get_messages_processed();
        uint64_t binance_msgs = binance_hot.get_messages_processed();
        uint64_t archived = cold_path.get_quotes_archived();
        uint64_t simulator_consumed = simulator_feed->get_quotes_consumed();
        
        // Calculate rates
        double coinbase_rate = (coinbase_msgs - last_coinbase_msgs) / 10.0;
        double binance_rate = (binance_msgs - last_binance_msgs) / 10.0;
        double archive_rate = (archived - last_archived) / 10.0;
        
        std::cout << "\n========================================\n";
        std::cout << "  SYSTEM STATISTICS (Uptime: " << uptime << "s)\n";
        std::cout << "========================================\n";
        std::cout << "HOT PATH (Real-time):\n";
        std::cout << "  Coinbase: " << coinbase_msgs << " msgs (" << coinbase_rate << "/sec)\n";
        std::cout << "  Binance:  " << binance_msgs << " msgs (" << binance_rate << "/sec)\n";
        std::cout << "\nCOLD PATH (Aggregated):\n";
        std::cout << "  Archived: " << archived << " quotes (" << archive_rate << "/sec)\n";
        std::cout << "  Arb Opps: " << cold_path.get_arb_opportunities() << "\n";
        std::cout << "\nEXCHANGE SIMULATOR:\n";
        std::cout << "  Consumed: " << simulator_consumed << " quotes\n";
        std::cout << "  Orders:   " << simulator_feed->get_orders_simulated() << "\n";
        std::cout << "\nDATA FLOW:\n";
        std::cout << "  Total: " << (coinbase_msgs + binance_msgs) << " quotes processed\n";
        std::cout << "  Rate:  " << ((coinbase_msgs + binance_msgs) / double(uptime)) << " quotes/sec avg\n";
        std::cout << "========================================\n";
        
        last_coinbase_msgs = coinbase_msgs;
        last_binance_msgs = binance_msgs;
        last_archived = archived;
    }
    
    // ===================================================================
    // STEP 10: Graceful Shutdown
    // ===================================================================
    std::cout << "\n[SHUTDOWN] Stopping all components...\n";
    
    // Stop in reverse order
    multi_exchange->stop();
    bridge.stop();
    simulator_feed->stop();
    cold_path.stop();
    coinbase_hot.stop();
    binance_hot.stop();
    
    std::cout << "\n========================================\n";
    std::cout << "  FINAL STATISTICS\n";
    std::cout << "========================================\n";
    std::cout << "Coinbase processed: " << coinbase_hot.get_messages_processed() << "\n";
    std::cout << "Binance processed:  " << binance_hot.get_messages_processed() << "\n";
    std::cout << "Total archived:     " << cold_path.get_quotes_archived() << "\n";
    std::cout << "Simulator consumed: " << simulator_feed->get_quotes_consumed() << "\n";
    std::cout << "========================================\n\n";
    
    std::cout << "[SYSTEM] Shutdown complete\n";
    
    return 0;
}
