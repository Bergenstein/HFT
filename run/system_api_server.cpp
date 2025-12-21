//==============================================================================
// HFT System API Server
// Provides ZeroMQ API for external strategy binaries (blackbox execution)
//==============================================================================

#include "api/market_data_server.hpp"
#include "core/timestamp.hpp"
#include <iostream>
#include <signal.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <mutex>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace hft::api;

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false); }

// CURL write callback
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Simple exchange data fetcher
class SimpleFundingFetcher {
public:
    struct FundingData {
        std::string exchange;
        std::string symbol;
        double funding_rate;
        double price;
        uint64_t timestamp_ns;
    };
    
    static bool fetch_binance_funding(const std::string& symbol, FundingData& out) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        std::string url = "https://fapi.binance.com/fapi/v1/premiumIndex?symbol=" + symbol;
        std::string response;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK || response.empty()) return false;
        
        try {
            json j = json::parse(response);
            out.exchange = "Binance";
            out.symbol = symbol;
            out.funding_rate = std::stod(j["lastFundingRate"].get<std::string>());
            out.price = std::stod(j["markPrice"].get<std::string>());
            out.timestamp_ns = core::Timestamp::to_nanos(core::Timestamp::now());
            return true;
        } catch (...) {
            return false;
        }
    }
    
    static bool fetch_bybit_funding(const std::string& symbol, FundingData& out) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        std::string url = "https://api.bybit.com/v5/market/tickers?category=linear&symbol=" + symbol;
        std::string response;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK || response.empty()) return false;
        
        try {
            json j = json::parse(response);
            if (!j["result"]["list"].empty()) {
                out.exchange = "Bybit";
                out.symbol = symbol;
                out.funding_rate = std::stod(j["result"]["list"][0]["fundingRate"].get<std::string>());
                out.price = std::stod(j["result"]["list"][0]["lastPrice"].get<std::string>());
                out.timestamp_ns = core::Timestamp::to_nanos(core::Timestamp::now());
                return true;
            }
        } catch (...) {
            return false;
        }
        return false;
    }
};

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    
    int runtime_sec = (argc > 1) ? std::stoi(argv[1]) : 60;
    
    std::cout << "==========================================================\n";
    std::cout << "  HFT SYSTEM API SERVER (BLACKBOX STRATEGY SUPPORT)\n";
    std::cout << "==========================================================\n\n";
    
    std::cout << "Initializing API server for external strategy binaries...\n\n";
    
    // Start API server
    MarketDataServer api_server("tcp://*:5555", "tcp://*:5556");
    
    std::cout << "✓ API Server initialized\n";
    std::cout << "  Market Data Feed:  tcp://*:5555 (PUB)\n";
    std::cout << "  Trading Signals:   tcp://*:5556 (PULL)\n";
    std::cout << "  Protocol:          JSON over ZeroMQ\n";
    std::cout << "  Strategy Support:  Blackbox binaries\n\n";
    
    // Register signal handler to receive signals from strategies
    std::atomic<uint64_t> signal_count{0};
    std::mutex signal_mutex;
    std::map<std::string, int> signals_by_strategy;
    
    api_server.on_signal([&](const TradingSignal& signal) {
        signal_count++;
        std::lock_guard<std::mutex> lock(signal_mutex);
        signals_by_strategy[signal.strategy_id]++;
        
        std::cout << "[SIGNAL #" << signal_count << "] "
                  << "Strategy: " << signal.strategy_id
                  << " | " << signal.action << " " << signal.symbol
                  << " | Qty: " << signal.quantity
                  << " | Exchange: " << signal.exchange << "\n";
    });
    
    api_server.start();
    std::cout << "✓ API Server started\n\n";
    
    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Start data collection
    std::cout << "Fetching market data from exchanges...\n";
    std::cout << "  • Binance (BTC/USDT, ETH/USDT)\n";
    std::cout << "  • Bybit (BTC/USDT, ETH/USDT)\n";
    std::cout << "  • Polling interval: 2000ms\n\n";
    
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT"};
    std::atomic<uint64_t> publish_count{0};
    
    std::cout << "Running for " << runtime_sec << " seconds...\n";
    std::cout << "External strategies can connect now.\n\n";
    
    auto start_time = std::chrono::steady_clock::now();
    
    while (g_running.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
        
        if (elapsed >= runtime_sec) break;
        
        // Fetch and publish data for each symbol
        for (const auto& symbol : symbols) {
            // Fetch from Binance
            SimpleFundingFetcher::FundingData data;
            if (SimpleFundingFetcher::fetch_binance_funding(symbol, data)) {
                MarketDataSnapshot snapshot;
                snapshot.exchange = data.exchange;
                snapshot.symbol = symbol;
                snapshot.timestamp_ns = data.timestamp_ns;
                snapshot.bid_price = data.price - 0.5;  // Simulated spread
                snapshot.ask_price = data.price + 0.5;
                snapshot.bid_qty = 1.0;
                snapshot.ask_qty = 1.0;
                snapshot.funding_rate = data.funding_rate;
                snapshot.funding_rate_annual = data.funding_rate * 365 * 3 * 100; // 8-hour to annual
                
                api_server.publish(snapshot);
                publish_count++;
            }
            
            // Fetch from Bybit
            if (SimpleFundingFetcher::fetch_bybit_funding(symbol, data)) {
                MarketDataSnapshot snapshot;
                snapshot.exchange = data.exchange;
                snapshot.symbol = symbol;
                snapshot.timestamp_ns = data.timestamp_ns;
                snapshot.bid_price = data.price - 0.5;
                snapshot.ask_price = data.price + 0.5;
                snapshot.bid_qty = 1.0;
                snapshot.ask_qty = 1.0;
                snapshot.funding_rate = data.funding_rate;
                snapshot.funding_rate_annual = data.funding_rate * 365 * 3 * 100;
                
                api_server.publish(snapshot);
                publish_count++;
            }
        }
        
        // Status update
        std::cout << "\r[Elapsed: " << elapsed << "s | Published: " << publish_count 
                  << " | Signals Received: " << signal_count << "]    " << std::flush;
        
        // Wait before next poll
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
    
    std::cout << "\n\nShutting down...\n";
    api_server.stop();
    curl_global_cleanup();
    
    std::cout << "\n==========================================================\n";
    std::cout << "  FINAL STATISTICS\n";
    std::cout << "==========================================================\n";
    std::cout << "Market Data Published:  " << publish_count << "\n";
    std::cout << "Signals Received:       " << signal_count << "\n";
    
    if (!signals_by_strategy.empty()) {
        std::cout << "\nSignals by Strategy:\n";
        std::lock_guard<std::mutex> lock(signal_mutex);
        for (const auto& [strategy, count] : signals_by_strategy) {
            std::cout << "  " << strategy << ": " << count << "\n";
        }
    }
    
    std::cout << "\n✓ System shutdown complete\n\n";
    std::cout << "Note: Strategy binaries were blackbox - system doesn't know\n";
    std::cout << "      what logic they used, only received their signals.\n\n";
    
    return 0;
}
