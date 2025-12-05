// Complete end-to-end latency measurement
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <vector>
#include <nlohmann/json.hpp>
#include "../md/normalizer.hpp"
#include "../md/order_book.hpp"
#include "../core/latency_tracker.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <ndjson_file> <product>\n";
        return 1;
    }

    const std::string filename = argv[1];
    const std::string target_product = argv[2];
    
    LatencyTracker parse_latency;
    LatencyTracker book_snapshot_latency;
    LatencyTracker book_update_latency;
    LatencyTracker total_latency;
    
    std::map<std::string, OrderBook> books;
    
    std::ifstream ifs(filename);
    std::string line;
    int line_count = 0;
    int snapshot_count = 0;
    int update_count = 0;
    
    std::cout << "=== Total LATENCY MEASUREMENT ===\n";
    std::cout << "File: " << filename << "\n";
    std::cout << "Product: " << target_product << "\n";
    std::cout << "Measuring: Parse + Order Book Updates\n\n";
    
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        line_count++;
        
        auto t0 = std::chrono::high_resolution_clock::now();
        
        // Extract raw JSON from recorder format
        json wrapper = json::parse(line, nullptr, false);
        if (wrapper.is_discarded() || !wrapper.contains("raw")) continue;
        std::string raw_json = wrapper["raw"].get<std::string>();
        
        // Parse the actual L2 message
        std::vector<L2Snapshot> snaps;
        std::vector<L2Update> updates;
        Normalizer::parse(raw_json, snaps, updates);
        
        auto t1 = std::chrono::high_resolution_clock::now();
        long long parse_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        parse_latency.add_sample(parse_ns);
        
        // Process snapshots
        for (auto& snap : snaps) {
            if (snap.product != target_product) continue;
            
            auto t2 = std::chrono::high_resolution_clock::now();
            books[snap.product].on_snapshot(snap);
            auto t3 = std::chrono::high_resolution_clock::now();
            
            long long snap_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
            book_snapshot_latency.add_sample(snap_ns);
            snapshot_count++;
        }
        
        // Process updates
        for (auto& upd : updates) {
            if (upd.product != target_product) continue;
            
            auto t2 = std::chrono::high_resolution_clock::now();
            books[upd.product].on_update(upd);
            auto t3 = std::chrono::high_resolution_clock::now();
            
            long long upd_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
            book_update_latency.add_sample(upd_ns);
            update_count++;
        }
        
        auto t_end = std::chrono::high_resolution_clock::now();
        long long total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t0).count();
        total_latency.add_sample(total_ns);
    }
    
    std::cout << "\n========== MEASURED LATENCY RESULTS ==========\n";
    std::cout << "Lines processed: " << line_count << "\n";
    std::cout << "Snapshots: " << snapshot_count << "\n";
    std::cout << "Updates: " << update_count << "\n\n";
    
    std::cout << "[1] PARSE (wrapper+JSON): ";
    parse_latency.print_stats();
    
    if (snapshot_count > 0) {
        std::cout << "[2] BOOK SNAPSHOT:        ";
        book_snapshot_latency.print_stats();
    }
    
    if (update_count > 0) {
        std::cout << "[3] BOOK UPDATE:          ";
        book_update_latency.print_stats();
    }
    
    std::cout << "[TOTAL System Latency] END-TO-END:       ";
    total_latency.print_stats();
    
    std::cout << "==============================================\n";
    std::cout << "   (Network latency not included)\n";
    
    return 0;
}
