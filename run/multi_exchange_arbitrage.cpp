// run/multi_exchange_arbitrage.cpp
#include "../exchanges/exchange_config.hpp"
#include "../exchanges/binance_ws_client.hpp"
#include "../exchanges/kraken_ws_client.hpp"
#include "../exchanges/okx_ws_client.hpp"
#include "../exchanges/bybit_ws_client.hpp"
#include "../exchanges/huobi_ws_client.hpp"
#include "../arb/multi_exchange_engine.hpp"
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>

std::atomic<bool> running{true};

void signal_handler(int signal) {
    std::cout << "\n[MAIN] Caught signal " << signal << ", shutting down...\n";
    running = false;
}

// Normalize Binance data to standard format
void handle_binance_message(const std::string& symbol, const nlohmann::json& data,
                            arb::MultiExchangeEngine& engine) {
    try {
        // Binance depth format: {"e": "depthUpdate", "b": [[price, qty], ...], "a": [[price, qty], ...]}
        if (!data.contains("b") || !data.contains("a")) return;
        
        auto bids = data["b"];
        auto asks = data["a"];
        
        if (bids.empty() || asks.empty()) return;
        
        // Get best bid/ask
        double best_bid = std::stod(bids[0][0].get<std::string>());
        double best_ask = std::stod(asks[0][0].get<std::string>());
        double bid_size = std::stod(bids[0][1].get<std::string>());
        double ask_size = std::stod(asks[0][1].get<std::string>());
        
        // Normalize symbol (BTCUSDT -> BTC-USDT for matching)
        std::string normalized = symbol;
        // Simple normalization - in production, use proper mapping
        if (normalized == "BTCUSDT") normalized = "BTC-USDT";
        else if (normalized == "ETHUSDT") normalized = "ETH-USDT";
        else if (normalized == "SOLUSDT") normalized = "SOL-USDT";
        
        engine.update_quote("binance", normalized, best_bid, best_ask, 
                          bid_size, ask_size, 50000); // 50ms typical latency
        
    } catch (const std::exception& e) {
        std::cerr << "[BINANCE] Error processing message: " << e.what() << "\n";
    }
}

// Normalize Kraken data
void handle_kraken_message(const std::string& symbol, const nlohmann::json& data,
                          arb::MultiExchangeEngine& engine) {
    try {
        // Kraken format: {"bs": [[price, volume, timestamp], ...], "as": [[price, volume, timestamp], ...]}
        if (!data.contains("bs") && !data.contains("as") &&
            !data.contains("b") && !data.contains("a")) return;
        
        auto bids = data.contains("bs") ? data["bs"] : data["b"];
        auto asks = data.contains("as") ? data["as"] : data["a"];
        
        if (bids.empty() || asks.empty()) return;
        
        double best_bid = std::stod(bids[0][0].get<std::string>());
        double best_ask = std::stod(asks[0][0].get<std::string>());
        double bid_size = std::stod(bids[0][1].get<std::string>());
        double ask_size = std::stod(asks[0][1].get<std::string>());
        
        // Normalize symbol (XBT/USD -> BTC-USD)
        std::string normalized = symbol;
        if (normalized == "XBTUSD" || normalized == "XBT/USD") normalized = "BTC-USD";
        else if (normalized == "ETHUSD" || normalized == "ETH/USD") normalized = "ETH-USD";
        
        engine.update_quote("kraken", normalized, best_bid, best_ask,
                          bid_size, ask_size, 100000); // 100ms typical latency
        
    } catch (const std::exception& e) {
        std::cerr << "[KRAKEN] Error processing message: " << e.what() << "\n";
    }
}

// Normalize OKX data
void handle_okx_message(const std::string& symbol, const nlohmann::json& data,
                       arb::MultiExchangeEngine& engine) {
    try {
        // OKX format: {"bids": [[price, size, liquidated, num_orders], ...], "asks": [...]}
        if (!data.contains("bids") || !data.contains("asks")) return;
        
        auto bids = data["bids"];
        auto asks = data["asks"];
        
        if (bids.empty() || asks.empty()) return;
        
        double best_bid = std::stod(bids[0][0].get<std::string>());
        double best_ask = std::stod(asks[0][0].get<std::string>());
        double bid_size = std::stod(bids[0][1].get<std::string>());
        double ask_size = std::stod(asks[0][1].get<std::string>());
        
        // OKX uses proper format already (BTC-USDT)
        engine.update_quote("okx", symbol, best_bid, best_ask,
                          bid_size, ask_size, 40000); // 40ms typical latency
        
    } catch (const std::exception& e) {
        std::cerr << "[OKX] Error processing message: " << e.what() << "\n";
    }
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    std::cout << R"(
╔══════════════════════════════════════════════════════════════════╗
║         MULTI-EXCHANGE ARBITRAGE ENGINE v1.0                     ║
║         Real-time cross-exchange opportunity detection           ║
╚══════════════════════════════════════════════════════════════════╝
)" << "\n";

    // Configuration
    double min_cross_profit_bps = 15.0;
    double min_tri_profit_bps = 20.0;
    double min_stat_z_score = 2.0;
    
    if (argc > 1) {
        min_cross_profit_bps = std::stod(argv[1]);
    }
    
    std::cout << "[CONFIG] Minimum cross-exchange profit: " << min_cross_profit_bps << " bps\n";
    std::cout << "[CONFIG] Minimum triangular arb profit: " << min_tri_profit_bps << " bps\n";
    std::cout << "[CONFIG] Statistical arb z-score threshold: " << min_stat_z_score << "\n\n";
    
    // Initialize arbitrage engine
    arb::MultiExchangeEngine arb_engine(min_cross_profit_bps, 
                                        min_tri_profit_bps,
                                        min_stat_z_score);
    arb_engine.start();
    
    // Products to monitor (focus on high-liquidity pairs)
    std::vector<std::string> products = {
        "BTC-USD", "BTC-USDT", "ETH-USD", "ETH-USDT", "SOL-USDT"
    };
    
    std::cout << "[MAIN] Monitoring products: ";
    for (const auto& p : products) std::cout << p << " ";
    std::cout << "\n\n";
    
    // Start Binance client
    std::thread binance_thread([&]() {
        md::BinanceWSClient binance_client;
        binance_client.set_message_callback(
            [&](const std::string& symbol, const nlohmann::json& data) {
                handle_binance_message(symbol, data, arb_engine);
            });
        
        std::vector<std::string> binance_symbols = {"btcusdt", "ethusdt", "solusdt"};
        std::cout << "[BINANCE] Subscribing to " << binance_symbols.size() << " symbols...\n";
        binance_client.subscribe_orderbook(binance_symbols);
    });
    
    // Start Kraken client
    std::thread kraken_thread([&]() {
        md::KrakenWSClient kraken_client;
        kraken_client.set_message_callback(
            [&](const std::string& symbol, const nlohmann::json& data) {
                handle_kraken_message(symbol, data, arb_engine);
            });
        
        std::vector<std::string> kraken_symbols = {"XBT/USD", "ETH/USD", "SOL/USD"};
        std::cout << "[KRAKEN] Subscribing to " << kraken_symbols.size() << " symbols...\n";
        kraken_client.subscribe_orderbook(kraken_symbols);
    });
    
    // Start OKX client
    std::thread okx_thread([&]() {
        md::OKXWSClient okx_client;
        okx_client.set_message_callback(
            [&](const std::string& symbol, const nlohmann::json& data) {
                handle_okx_message(symbol, data, arb_engine);
            });
        
        std::vector<std::string> okx_symbols = {"BTC-USDT", "ETH-USDT", "SOL-USDT"};
        std::cout << "[OKX] Subscribing to " << okx_symbols.size() << " symbols...\n";
        okx_client.subscribe_orderbook(okx_symbols);
    });
    
    std::cout << "\n[MAIN] All exchange clients started\n";
    std::cout << "[MAIN] Waiting for data... (Ctrl+C to exit)\n\n";
    
    // Main loop - print opportunities every 10 seconds
    int iterations = 0;
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        if (!running) break;
        
        iterations++;
        
        // Print opportunities
        arb_engine.print_opportunities();
        
        // Print statistics
        auto stats = arb_engine.get_stats();
        std::cout << "[STATS] Iteration " << iterations 
                  << " | Opportunities: " << stats.opportunities_found
                  << " | Best: " << stats.best_opportunity_bps << " bps"
                  << " | Total Potential: $" << stats.total_potential_profit_usd
                  << "\n\n";
    }
    
    std::cout << "\n[MAIN] Shutting down...\n";
    arb_engine.stop();
    
    // Note: WebSocket threads will be terminated when main exits
    // In production, implement proper cleanup
    
    std::cout << "[MAIN] Shutdown complete\n";
    
    return 0;
}
