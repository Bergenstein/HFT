//==============================================================================
// INTEGRATED MULTI-EXCHANGE PIPELINE TEST
//==============================================================================
// Tests the complete pipeline: REST fetcher → SPSC queues → Aggregator → Storage
// Demonstrates both hot path (real-time) and cold path (persistence)

#include "arb/multi_exchange_integration.hpp"
#include <iostream>
#include <iomanip>
#include <signal.h>

using namespace arb;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\n[Signal] Received interrupt, shutting down...\n";
    g_running.store(false);
}

//==============================================================================
// OPPORTUNITY MONITOR (Prints arbitrage opportunities)
//==============================================================================

class OpportunityMonitor {
public:
    OpportunityMonitor(MultiExchangeSystem& system) : system_(system) {}
    
    void start() {
        monitor_thread_ = std::thread([this]() {
            std::cout << "[Monitor] Started opportunity monitor\n";
            
            while (g_running.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                
                auto opportunities = system_.find_opportunities(20.0);
                
                if (opportunities.empty()) {
                    std::cout << "\n[Monitor] No opportunities found (>20% APY)\n";
                    continue;
                }
                
                std::cout << "\n╔════════════════════════════════════════════════════════════════╗\n";
                std::cout << "║           FUNDING RATE ARBITRAGE OPPORTUNITIES                 ║\n";
                std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
                
                int count = 0;
                for (const auto& opp : opportunities) {
                    if (++count > 10) break;  // Show top 10
                    
                    std::cout << "\n[" << count << "] " << opp.symbol.to_string() 
                              << " - Net APY: " << std::fixed << std::setprecision(2) 
                              << opp.net_profit_apy << "%\n";
                    
                    std::cout << "  Long:  " << exchange_to_string(opp.long_exchange) 
                              << " @ " << opp.long_funding_rate_annual << "% APY"
                              << " (price: $" << opp.long_price << ")\n";
                    
                    std::cout << "  Short: " << exchange_to_string(opp.short_exchange) 
                              << " @ " << opp.short_funding_rate_annual << "% APY"
                              << " (price: $" << opp.short_price << ")\n";
                    
                    std::cout << "  Spread: " << opp.spread_annual << "% APY\n";
                    std::cout << "  Price diff: " << opp.price_difference_bps << " bps\n";
                    std::cout << "  Liquidity: $" << opp.liquidity_score << "\n";
                    std::cout << "  Available on " << opp.num_exchanges_with_symbol << " exchanges\n";
                }
                
                system_.print_stats();
            }
        });
    }
    
    void stop() {
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }
    
private:
    MultiExchangeSystem& system_;
    std::thread monitor_thread_;
};

//==============================================================================
// MARKET DATA CALLBACK (Processes every update)
//==============================================================================

class MarketDataProcessor {
public:
    void on_market_data(const UnifiedMarketData& data) {
        updates_received_++;
        
        // Log every 1000th update
        if (updates_received_ % 1000 == 0) {
            std::cout << "[Processor] Received " << updates_received_ << " updates\n";
            std::cout << "  Latest: " << exchange_to_string(data.orderbook.exchange_id)
                      << " " << data.orderbook.unified_symbol.to_string()
                      << " mid=$" << data.orderbook.mid_price()
                      << " spread=" << data.orderbook.spread_bps() << "bps";
            
            if (data.has_funding()) {
                std::cout << " funding=" << std::fixed << std::setprecision(4)
                          << data.funding->funding_rate_annual << "%APY";
            }
            std::cout << "\n";
        }
        
        // Track latency
        auto latency_ns = data.orderbook.latency_ns();
        if (latency_ns > max_latency_ns_) {
            max_latency_ns_ = latency_ns;
        }
        total_latency_ns_ += latency_ns;
    }
    
    void print_summary() const {
        std::cout << "\n=== Market Data Processor Summary ===\n";
        std::cout << "Total updates: " << updates_received_ << "\n";
        std::cout << "Average latency: " << (total_latency_ns_ / updates_received_ / 1000) << " us\n";
        std::cout << "Max latency: " << (max_latency_ns_ / 1000) << " us\n";
    }
    
private:
    std::atomic<uint64_t> updates_received_{0};
    std::atomic<int64_t> max_latency_ns_{0};
    std::atomic<int64_t> total_latency_ns_{0};
};

//==============================================================================
// MAIN TEST
//==============================================================================

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║    INTEGRATED MULTI-EXCHANGE PIPELINE TEST                     ║\n";
    std::cout << "║    Hot Path: SPSC Queues | Cold Path: SQLite Storage          ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    
    // Register signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        // Parse command line arguments
        bool enable_storage = true;
        int runtime_seconds = 60;  // Default 1 minute
        std::string db_path = "integrated_test.db";
        
        if (argc > 1) {
            runtime_seconds = std::stoi(argv[1]);
        }
        if (argc > 2) {
            enable_storage = (std::string(argv[2]) == "true");
        }
        if (argc > 3) {
            db_path = argv[3];
        }
        
        std::cout << "[Config] Runtime: " << runtime_seconds << " seconds\n";
        std::cout << "[Config] Storage: " << (enable_storage ? "enabled" : "disabled") << "\n";
        if (enable_storage) {
            std::cout << "[Config] Database: " << db_path << "\n";
        }
        std::cout << "\n";
        
        // Build multi-exchange system
        std::cout << "[Setup] Building multi-exchange system...\n";
        
        auto system = MultiExchangeSystemBuilder()
            .with_binance(1000)     // Poll every 1 second
            .with_bybit(1000)
            .with_gateio(1000)
            .with_mexc(1500)
            .with_kucoin(1500)
            .with_okx(2000)         // OKX slower due to rate limits
            .with_kraken(1500)
            .with_bitget(1000)
            .with_htx(1500)
            .build();
        
        std::cout << "[Setup] System built successfully\n\n";
        
        // Create market data processor
        MarketDataProcessor processor;
        system->on_market_data([&processor](const UnifiedMarketData& data) {
            processor.on_market_data(data);
        });
        
        // Start the system
        std::cout << "[System] Starting all exchange feeders...\n";
        system->start(enable_storage, db_path, -1);  // -1 = no CPU affinity
        
        std::cout << "[System] All feeders started\n\n";
        
        // Start opportunity monitor
        OpportunityMonitor monitor(*system);
        monitor.start();
        
        // Run for specified time
        std::cout << "[System] Running for " << runtime_seconds << " seconds...\n";
        std::cout << "[System] Press Ctrl+C to stop early\n\n";
        
        auto start_time = std::chrono::steady_clock::now();
        while (g_running.load()) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= runtime_seconds) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Graceful shutdown
        std::cout << "\n[System] Initiating shutdown...\n";
        
        monitor.stop();
        system->stop();
        
        // Print final statistics
        std::cout << "\n";
        std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                    FINAL STATISTICS                            ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
        
        system->print_stats();
        processor.print_summary();
        
        // Print final opportunities
        auto final_opps = system->find_opportunities(20.0);
        std::cout << "\n[Final] Found " << final_opps.size() << " opportunities (>20% APY)\n";
        
        if (!final_opps.empty()) {
            std::cout << "\nTop 5 Opportunities:\n";
            for (int i = 0; i < std::min(5, (int)final_opps.size()); i++) {
                const auto& opp = final_opps[i];
                std::cout << "  " << (i+1) << ". " << opp.symbol.to_string() 
                          << ": " << std::fixed << std::setprecision(2)
                          << opp.net_profit_apy << "% APY"
                          << " (Long: " << exchange_to_string(opp.long_exchange)
                          << ", Short: " << exchange_to_string(opp.short_exchange) << ")\n";
            }
        }
        
        std::cout << "\n[System] Shutdown complete\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
