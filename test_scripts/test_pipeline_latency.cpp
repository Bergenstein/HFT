#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <thread>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "../arb/normalized_exchange_data.hpp"
#include "../pipeline/spsc_queue.hpp"

using json = nlohmann::json;
using namespace arb;
using namespace pipeline;

struct LatencyBreakdown {
    long long fetch_ns;
    long long parse_ns;
    long long normalize_ns;
    long long queue_push_ns;
    long long total_ns;
};

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    size_t total = size * nmemb;
    out->append((char*)contents, total);
    return total;
}

std::string fetch_binance_data() {
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/ticker/bookTicker?symbol=BTCUSDT");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    
    return response;
}

NormalizedOrderbookSnapshot normalize_binance(const json& data) {
    NormalizedOrderbookSnapshot snap;
    snap.exchange_id = ExchangeID::BINANCE;
    snap.local_timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
    snap.exchange_timestamp_ns = snap.local_timestamp_ns;
    snap.sequence = 0;
    
    std::string symbol = data.value("symbol", "");
    snap.exchange_symbol = symbol;
    std::string base = symbol.substr(0, symbol.find("USDT"));
    snap.unified_symbol = UnifiedSymbol{base, "USDT"};
    
    snap.best_bid_price = std::stod(data.value("bidPrice", "0"));
    snap.best_bid_qty = std::stod(data.value("bidQty", "0"));
    snap.best_ask_price = std::stod(data.value("askPrice", "0"));
    snap.best_ask_qty = std::stod(data.value("askQty", "0"));
    
    snap.bids.push_back({snap.best_bid_price, snap.best_bid_qty});
    snap.asks.push_back({snap.best_ask_price, snap.best_ask_qty});
    
    return snap;
}

int main() {
    std::cout << "Pipeline Latency Test\n";
    std::cout << "Measuring: fetch -> parse -> normalize -> queue push\n\n";
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Create SPSC queue
    SPSCQueue<NormalizedOrderbookSnapshot> queue(1024);
    
    std::vector<LatencyBreakdown> samples;
    const int num_samples = 50;
    
    std::cout << "Collecting " << num_samples << " samples...\n";
    
    for (int i = 0; i < num_samples; i++) {
        LatencyBreakdown lat;
        
        // 1. Fetch
        auto t0 = std::chrono::high_resolution_clock::now();
        std::string response = fetch_binance_data();
        auto t1 = std::chrono::high_resolution_clock::now();
        lat.fetch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        
        if (response.empty()) {
            std::cout << "Fetch failed, skipping sample\n";
            continue;
        }
        
        // 2. Parse
        auto t2 = std::chrono::high_resolution_clock::now();
        json data = json::parse(response, nullptr, false);
        auto t3 = std::chrono::high_resolution_clock::now();
        lat.parse_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
        
        if (data.is_discarded()) {
            std::cout << "Parse failed, skipping sample\n";
            continue;
        }
        
        // 3. Normalize
        auto t4 = std::chrono::high_resolution_clock::now();
        NormalizedOrderbookSnapshot snap = normalize_binance(data);
        auto t5 = std::chrono::high_resolution_clock::now();
        lat.normalize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t5 - t4).count();
        
        // 4. Queue push
        auto t6 = std::chrono::high_resolution_clock::now();
        bool pushed = queue.try_push(std::move(snap));
        auto t7 = std::chrono::high_resolution_clock::now();
        lat.queue_push_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t7 - t6).count();
        
        if (!pushed) {
            std::cout << "Queue full, skipping sample\n";
            continue;
        }
        
        lat.total_ns = lat.fetch_ns + lat.parse_ns + lat.normalize_ns + lat.queue_push_ns;
        samples.push_back(lat);
        
        // Rate limit
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    curl_global_cleanup();
    
    if (samples.empty()) {
        std::cout << "No samples collected\n";
        return 1;
    }
    
    // Calculate statistics
    auto calc_stats = [](std::vector<long long>& values) {
        std::sort(values.begin(), values.end());
        return std::make_tuple(
            values.front(),
            values[values.size() / 2],
            values[values.size() * 95 / 100],
            values[values.size() * 99 / 100],
            values.back()
        );
    };
    
    std::vector<long long> fetch_times, parse_times, normalize_times, queue_times, total_times;
    for (const auto& s : samples) {
        fetch_times.push_back(s.fetch_ns);
        parse_times.push_back(s.parse_ns);
        normalize_times.push_back(s.normalize_ns);
        queue_times.push_back(s.queue_push_ns);
        total_times.push_back(s.total_ns);
    }
    
    auto [f_min, f_p50, f_p95, f_p99, f_max] = calc_stats(fetch_times);
    auto [p_min, p_p50, p_p95, p_p99, p_max] = calc_stats(parse_times);
    auto [n_min, n_p50, n_p95, n_p99, n_max] = calc_stats(normalize_times);
    auto [q_min, q_p50, q_p95, q_p99, q_max] = calc_stats(queue_times);
    auto [t_min, t_p50, t_p95, t_p99, t_max] = calc_stats(total_times);
    
    std::cout << "\nSYSTEM LATENCY (p50 median):\n";
    std::cout << "============================\n\n";
    
    std::cout << "WITH NETWORK:\n";
    std::cout << "-------------\n";
    std::cout << "REST fetch: " << f_p50 << " ns\n";
    std::cout << "JSON parse: " << p_p50 << " ns\n";
    std::cout << "Normalize: " << n_p50 << " ns\n";
    std::cout << "SPSC push: " << q_p50 << " ns\n";
    std::cout << "           --------\n";
    std::cout << "Total: " << t_p50 << " ns\n\n";
    
    long long local_only = p_p50 + n_p50 + q_p50;
    std::cout << "WITHOUT NETWORK (local processing):\n";
    std::cout << "-----------------------------------\n";
    std::cout << "JSON parse: " << p_p50 << " ns\n";
    std::cout << "Normalize: " << n_p50 << " ns\n";
    std::cout << "SPSC push: " << q_p50 << " ns\n";
    std::cout << "           --------\n";
    std::cout << "Total: " << local_only << " ns\n\n";
    
    std::cout << "PERCENTAGE BREAKDOWN (p50):\n";
    std::cout << "---------------------------\n";
    std::cout << "REST fetch: " << (100.0 * f_p50 / t_p50) << "%\n";
    std::cout << "JSON parse: " << (100.0 * p_p50 / t_p50) << "%\n";
    std::cout << "Normalize: " << (100.0 * n_p50 / t_p50) << "%\n";
    std::cout << "SPSC push: " << (100.0 * q_p50 / t_p50) << "%\n";
    
    return 0;
}
