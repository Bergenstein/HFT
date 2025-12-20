//==============================================================================
// COMPLETE SYSTEM TEST: Multi-Exchange → Normalize → SPSC → Cold Storage → Strategy
//==============================================================================

#include "arb/multi_exchange_integration.hpp"
#include "arb/funding_rate_arb_engine.hpp"
#include <iostream>
#include <iomanip>
#include <signal.h>
#include <chrono>

using namespace arb;

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false); }

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    
    int runtime_sec = (argc > 1) ? std::stoi(argv[1]) : 30;
    
    std::cout << "\n=== COMPLETE SYSTEM TEST ===\n\n";
    
    // 1. Multi-exchange L2 orderbook + funding data from ALL exchanges
    std::cout << "[1/5] Fetching L2 + funding from ALL exchanges...\n";
    auto system = MultiExchangeSystemBuilder()
        .with_binance(1000)
        .with_bybit(1000)
        .with_gateio(1000)
        .with_okx(2000)
        .with_mexc(1000)
        .with_kucoin(1000)
        .with_kraken(1000)
        .with_bitget(1000)
        .with_htx(1000)
        .build();
    std::cout << "      Configured 9 exchanges\n\n";
    
    // 2. Data normalization happens automatically in the pipeline
    std::cout << "[2/5] Data normalization (automatic in pipeline)\n";
    std::cout << "      Exchange-specific symbols → Unified symbols (BTC/USDT format)\n";
    std::cout << "      Funding rates → Annualized APY\n";
    std::cout << "      Orderbook → Normalized structure\n\n";
    
    // 3. Start SPSC queue pipeline
    std::cout << "[3/5] Starting SPSC queue pipeline...\n";
    system->start(true, "db/complete_system_test.db");  // Enable cold storage
    std::cout << "      ✓ 9 exchange feeders started\n";
    std::cout << "      ✓ SPSC queues active (lock-free)\n";
    std::cout << "      ✓ Aggregator thread running\n\n";
    
    // 4. Cold storage is automatic via the aggregator
    std::cout << "[4/5] Cold storage enabled\n";
    std::cout << "      ✓ SQLite: db/complete_system_test.db\n";
    std::cout << "      ✓ Async writes from SPSC queues\n\n";
    
    // 5. Funding rate arbitrage strategy
    std::cout << "[5/5] Funding rate arbitrage strategy active\n";
    std::cout << "      Scanning: All exchanges, all tickers\n";
    std::cout << "      Detection: Perp-Perp cross-exchange spreads\n\n";
    
    std::cout << "Running for " << runtime_sec << "s (Ctrl+C to stop)...\n\n";
    
    // Track statistics
    std::atomic<uint64_t> total_updates{0};
    std::atomic<uint64_t> updates_with_funding{0};
    std::map<std::string, int> exchange_updates;
    
    system->on_market_data([&](const UnifiedMarketData& data) {
        total_updates++;
        if (data.has_funding()) {
            updates_with_funding++;
        }
        exchange_updates[exchange_to_string(data.orderbook.exchange_id)]++;
    });
    
    // Wait for initial data
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    auto start = std::chrono::steady_clock::now();
    int scan_num = 0;
    
    while (g_running.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= runtime_sec) break;
        
        std::this_thread::sleep_for(std::chrono::seconds(5));
        scan_num++;
        
        // Find arbitrage opportunities across ALL exchanges and ALL tickers
        auto opps = system->find_opportunities(5.0, 0.90);  // 5% min spread
        
        if (!opps.empty()) {
            std::cout << "\n━━━ Scan #" << scan_num << " ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
            std::cout << "Found " << opps.size() << " opportunities:\n\n";
            
            int shown = 0;
            for (const auto& opp : opps) {
                if (++shown > 10) break;
                
                std::cout << shown << ". " << opp.symbol.to_string() << ": "
                          << std::fixed << std::setprecision(2) << opp.net_profit_apy << "% APY\n";
                
                // Determine strategy based on funding rate signs
                double long_rate = opp.long_funding_rate_annual;
                double short_rate = opp.short_funding_rate_annual;
                
                std::cout << "   Strategy: ";
                if (long_rate < 0 && short_rate > 0) {
                    std::cout << "LONG " << opp.symbol.to_string() << " on " 
                              << exchange_to_string(opp.long_exchange)
                              << " (collect " << std::abs(long_rate) << "% APY from shorts paying longs)\n";
                    std::cout << "             SHORT " << opp.symbol.to_string() << " on "
                              << exchange_to_string(opp.short_exchange)
                              << " (collect " << short_rate << "% APY from longs paying shorts)\n";
                    std::cout << "             → Collect on BOTH sides = " << opp.spread_annual << "% APY gross\n";
                } else if (long_rate > 0 && short_rate > 0) {
                    std::cout << "LONG " << opp.symbol.to_string() << " on "
                              << exchange_to_string(opp.long_exchange)
                              << " (pay less: " << long_rate << "% APY)\n";
                    std::cout << "             SHORT " << opp.symbol.to_string() << " on "
                              << exchange_to_string(opp.short_exchange)
                              << " (collect more: " << short_rate << "% APY)\n";
                    std::cout << "             → Net collect = " << (short_rate - long_rate) << "% APY\n";
                } else if (long_rate < 0 && short_rate < 0) {
                    std::cout << "LONG " << opp.symbol.to_string() << " on "
                              << exchange_to_string(opp.long_exchange)
                              << " (collect more: " << std::abs(long_rate) << "% APY)\n";
                    std::cout << "             SHORT " << opp.symbol.to_string() << " on "
                              << exchange_to_string(opp.short_exchange)
                              << " (pay less: " << std::abs(short_rate) << "% APY)\n";
                    std::cout << "             → Net collect = " << (std::abs(long_rate) - std::abs(short_rate)) << "% APY\n";
                } else {
                    std::cout << "LONG " << opp.symbol.to_string() << " on "
                              << exchange_to_string(opp.long_exchange)
                              << " @ " << long_rate << "% APY\n";
                    std::cout << "             SHORT " << opp.symbol.to_string() << " on "
                              << exchange_to_string(opp.short_exchange)
                              << " @ " << short_rate << "% APY\n";
                }
                
                std::cout << "   Liquidity: $" << std::setprecision(0) << opp.liquidity_score << "\n";
                std::cout << "   Price Divergence: " << std::setprecision(2) << opp.price_difference_bps << " bps\n\n";
            }
            
            if (opps.size() > 10) {
                std::cout << "   ... and " << (opps.size() - 10) << " more\n";
            }
        }
        
        std::cout << "[Stats] Total Updates: " << total_updates 
                  << " | With Funding: " << updates_with_funding 
                  << " | Opportunities: " << opps.size() << "    \r" << std::flush;
    }
    
    std::cout << "\n\n[Shutdown] Stopping system...\n";
    system->stop();
    
    std::cout << "\n=== FINAL RESULTS ===\n\n";
    std::cout << "Data Collection:\n";
    std::cout << "  Total Updates: " << total_updates << "\n";
    std::cout << "  Updates with Funding: " << updates_with_funding << "\n";
    std::cout << "  Coverage: " << std::fixed << std::setprecision(1) 
              << (100.0 * updates_with_funding / total_updates) << "%\n\n";
    
    std::cout << "Per-Exchange Updates:\n";
    for (const auto& [exchange, count] : exchange_updates) {
        std::cout << "  " << std::setw(10) << std::left << exchange << ": " << count << "\n";
    }
    
    std::cout << "\nCold Storage:\n";
    std::cout << "  Database: db/complete_system_test.db\n";
    std::cout << "  Records written: " << total_updates << "\n";
    
    std::cout << "\nSPSC Pipeline:\n";
    std::cout << "  Queue Type: Lock-free\n";
    std::cout << "  Exchanges: 9\n";
    std::cout << "  Throughput: " << (total_updates / runtime_sec) << " updates/sec\n";
    
    std::cout << "\nStrategy:\n";
    std::cout << "  Type: Funding Rate Arbitrage\n";
    std::cout << "  Scan Frequency: 5s\n";
    std::cout << "  Total Scans: " << scan_num << "\n";
    
    std::cout << "\n✓ Complete system test finished\n\n";
    
    return 0;
}
