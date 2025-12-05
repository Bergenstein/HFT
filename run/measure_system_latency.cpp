// Complete system latency measurement
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include "../md/normalizer.hpp"
#include "../md/order_book.hpp"
#include "../core/order_book.hpp"
#include "../strats/imbalance_taker.hpp"

using hrc = std::chrono::high_resolution_clock;
using ns = std::chrono::nanoseconds;

struct LatencyBreakdown {
    long long parse_ns = 0;
    long long book_update_ns = 0;
    long long strategy_ns = 0;
    long long total_ns = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <ndjson_file>\n";
        return 1;
    }

    std::vector<LatencyBreakdown> measurements;
    measurements.reserve(10000);
    
    Normalizer norm;
    OrderBook mdbook;
    core::OrderBook corebook;
    ImbalanceTaker strategy(0.6, 150);
    
    std::ifstream ifs(argv[1]);
    std::string line;
    int msg_count = 0;
    
    std::cout << "Measuring COMPLETE system latency:\n";
    std::cout << "  1. JSON Parse\n";
    std::cout << "  2. Order Book Update\n";
    std::cout << "  3. Strategy Decision\n";
    std::cout << "  4. TOTAL (1+2+3)\n\n";
    
    while (std::getline(ifs, line) && msg_count < 5000) {
        if (line.empty()) continue;
        
        auto t0 = hrc::now();
        
        // 1. Parse JSON
        std::vector<L2Snapshot> snaps;
        std::vector<L2Update> updates;
        Normalizer::parse(line, snaps, updates);
        auto t1 = hrc::now();
        
        // 2. Update order books
        for (auto& s : snaps) {
            mdbook.on_snapshot(s);
            corebook.bids.clear();
            corebook.asks.clear();
            for (auto& [px, qty] : s.bids) corebook.bids[px] = qty;
            for (auto& [px, qty] : s.asks) corebook.asks[px] = qty;
        }
        for (auto& u : updates) {
            mdbook.on_update(u);
            auto& side = u.is_bid ? corebook.bids : corebook.asks;
            if (u.qty <= 0) side.erase(u.price);
            else side[u.price] = u.qty;
        }
        auto t2 = hrc::now();
        
        // 3. Strategy decision
        TickContext tc;
        tc.tick = msg_count;
        tc.time_sec = 0.0;
        strategy.on_tick(tc, corebook);
        auto t3 = hrc::now();
        
        LatencyBreakdown lb;
        lb.parse_ns = std::chrono::duration_cast<ns>(t1 - t0).count();
        lb.book_update_ns = std::chrono::duration_cast<ns>(t2 - t1).count();
        lb.strategy_ns = std::chrono::duration_cast<ns>(t3 - t2).count();
        lb.total_ns = std::chrono::duration_cast<ns>(t3 - t0).count();
        
        measurements.push_back(lb);
        msg_count++;
        
        if (msg_count % 1000 == 0) {
            std::cout << "Processed " << msg_count << " messages...\n";
        }
    }
    
    // Calculate statistics
    if (measurements.empty()) {
        std::cerr << "No measurements!\n";
        return 1;
    }
    
    std::sort(measurements.begin(), measurements.end(), 
              [](const auto& a, const auto& b) { return a.total_ns < b.total_ns; });
    
    size_t n = measurements.size();
    auto calc_stats = [n](const auto& getter) {
        std::vector<long long> vals;
        for (const auto& m : measurements) vals.push_back(getter(m));
        std::sort(vals.begin(), vals.end());
        return std::make_tuple(
            vals.front(),                    // min
            vals[n/2],                       // p50
            vals[(n*95)/100],                // p95
            vals[(n*99)/100],                // p99
            vals.back(),                     // max
            std::accumulate(vals.begin(), vals.end(), 0LL) / (double)n  // avg
        );
    };
    
    auto [parse_min, parse_p50, parse_p95, parse_p99, parse_max, parse_avg] = 
        calc_stats([](const auto& m) { return m.parse_ns; });
    auto [book_min, book_p50, book_p95, book_p99, book_max, book_avg] = 
        calc_stats([](const auto& m) { return m.book_update_ns; });
    auto [strat_min, strat_p50, strat_p95, strat_p99, strat_max, strat_avg] = 
        calc_stats([](const auto& m) { return m.strategy_ns; });
    auto [total_min, total_p50, total_p95, total_p99, total_max, total_avg] = 
        calc_stats([](const auto& m) { return m.total_ns; });
    
    std::cout << "\n╔════════════════════════════════════════════════════════════╗\n";
    std::cout <<   "║          SYSTEM LATENCY BREAKDOWN                     ║\n";
    std::cout <<   "╠════════════════════════════════════════════════════════════╣\n";
    std::cout << std::fixed << std::setprecision(2);
    
    std::cout << "║ 1. JSON PARSING                                            ║\n";
    std::cout << "║    min=" << (parse_min/1000.0) << "μs"
              << " avg=" << (parse_avg/1000.0) << "μs"
              << " p50=" << (parse_p50/1000.0) << "μs"
              << " p95=" << (parse_p95/1000.0) << "μs"
              << " max=" << (parse_max/1000.0) << "μs ║\n";
    
    std::cout << "║ 2. ORDER BOOK UPDATE                                       ║\n";
    std::cout << "║    min=" << (book_min/1000.0) << "μs"
              << " avg=" << (book_avg/1000.0) << "μs"
              << " p50=" << (book_p50/1000.0) << "μs"
              << " p95=" << (book_p95/1000.0) << "μs"
              << " max=" << (book_max/1000.0) << "μs ║\n";
    
    std::cout << "║ 3. STRATEGY DECISION                                       ║\n";
    std::cout << "║    min=" << (strat_min/1000.0) << "μs"
              << " avg=" << (strat_avg/1000.0) << "μs"
              << " p50=" << (strat_p50/1000.0) << "μs"
              << " p95=" << (strat_p95/1000.0) << "μs"
              << " max=" << (strat_max/1000.0) << "μs ║\n";
    
    std::cout << "╠════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ TOTAL PROCESSING                            ║\n";
    std::cout << "║    min=" << (total_min/1000.0) << "μs"
              << " avg=" << (total_avg/1000.0) << "μs"
              << " p50=" << (total_p50/1000.0) << "μs"
              << " p95=" << (total_p95/1000.0) << "μs"
              << " max=" << (total_max/1000.0) << "μs ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n";
    
    std::cout << "\nMessages processed: " << msg_count << "\n";
    std::cout << "\n✅ This is system's processing time.\n";
    std::cout << "⚠️  Network latency is NOT included.\n";
    
    return 0;
}
