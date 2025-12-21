//==============================================================================
// HFT System with API Server
// Runs multi-exchange pipeline and exposes API for strategy binaries
//==============================================================================

#include "exchanges/multi_exchange_connector.hpp"
#include "pipeline/normalizer.hpp"
#include "pipeline/queue.hpp"
#include "api/market_data_server.hpp"
#include <iostream>
#include <signal.h>
#include <chrono>
#include <thread>
#include <sqlite3.h>

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false); }

using namespace hft::api;

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    
    int runtime_sec = (argc > 1) ? std::stoi(argv[1]) : 60;
    
    std::cout << "==========================================================\n";
    std::cout << "  HFT SYSTEM - MARKET DATA API SERVER\n";
    std::cout << "==========================================================\n\n";
    
    // Start API server
    MarketDataServer api_server("tcp://*:5555", "tcp://*:5556");
    
    std::cout << "✓ API Server initialized\n";
    std::cout << "  Market Data: tcp://*:5555 (PUB)\n";
    std::cout << "  Signals:     tcp://*:5556 (PULL)\n\n";
    
    // Register signal handler
    api_server.on_signal([](const TradingSignal& signal) {
        std::cout << "[SIGNAL] Strategy: " << signal.strategy_id
                  << " | Action: " << signal.action
                  << " | Symbol: " << signal.symbol
                  << " | Qty: " << signal.quantity
                  << " | Exchange: " << signal.exchange << "\n";
    });
    
    api_server.start();
    std::cout << "✓ API Server started\n\n";
    
    // Start multi-exchange system
    std::cout << "Starting multi-exchange data pipeline...\n";
    
    auto system = arb::MultiExchangeSystemBuilder()
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
    
    const std::string db_path = "db/hft_system_with_api.db";
    system->start(true, db_path);
    
    std::cout << "✓ Multi-exchange system started\n";
    std::cout << "  Exchanges: 9 (Binance, Bybit, Gate.io, OKX, MEXC, KuCoin, Kraken, Bitget, HTX)\n";
    std::cout << "  Database: " << db_path << "\n\n";
    
    std::atomic<uint64_t> total_updates{0};
    std::atomic<uint64_t> api_publishes{0};
    
    // Forward market data to API
    system->on_market_data([&](const arb::UnifiedMarketData& data) {
        total_updates++;
        
        // Convert to API format and publish
        MarketDataSnapshot snapshot;
        snapshot.exchange = arb::exchange_to_string(data.orderbook.exchange_id);
        snapshot.symbol = data.orderbook.unified_symbol.to_string();
        snapshot.timestamp_ns = data.orderbook.timestamp_ns;
        snapshot.bid_price = data.orderbook.bid_price;
        snapshot.ask_price = data.orderbook.ask_price;
        snapshot.bid_qty = data.orderbook.bid_qty;
        snapshot.ask_qty = data.orderbook.ask_qty;
        
        if (data.has_funding()) {
            snapshot.funding_rate = data.funding_rate.rate;
            snapshot.funding_rate_annual = data.funding_rate.rate_annual;
        } else {
            snapshot.funding_rate = 0.0;
            snapshot.funding_rate_annual = 0.0;
        }
        
        api_server.publish(snapshot);
        api_publishes++;
    });
    
    std::cout << "Running for " << runtime_sec << " seconds...\n";
    std::cout << "Strategy binaries can connect to:\n";
    std::cout << "  - Subscribe: tcp://localhost:5555\n";
    std::cout << "  - Signal:    tcp://localhost:5556\n\n";
    
    auto start = std::chrono::steady_clock::now();
    
    while (g_running.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= runtime_sec) break;
        
        std::cout << "\r[Updates: " << total_updates 
                  << " | API Publishes: " << api_publishes << "]    " << std::flush;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::cout << "\n\nShutting down...\n";
    system->stop();
    api_server.stop();
    
    std::cout << "\n==========================================================\n";
    std::cout << "  FINAL STATISTICS\n";
    std::cout << "==========================================================\n";
    std::cout << "Total Updates:     " << total_updates << "\n";
    std::cout << "API Publishes:     " << api_publishes << "\n";
    std::cout << "Database:          " << db_path << "\n";
    std::cout << "\n✓ System shutdown complete\n\n";
    
    return 0;
}
