#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <vector>
#include "../md/normalizer.hpp"
#include "../core/latency_tracker.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <ndjson_file>\n";
        return 1;
    }

    LatencyTracker parse_latency;
    std::vector<L2Snapshot> snaps;
    std::vector<L2Update> updates;
    
    std::ifstream ifs(argv[1]);
    std::string line;
    int line_count = 0;
    
    std::cout << "Measuring JSON parsing latency on " << argv[1] << "\n\n";
    
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        line_count++;
        
        auto t0 = std::chrono::high_resolution_clock::now();
        Normalizer::parse(line, snaps, updates);
        auto t1 = std::chrono::high_resolution_clock::now();
        
        long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        parse_latency.add_sample(ns);
        
        if (line_count >= 10000) break;
    }
    
    std::cout << "Processed " << line_count << " lines\n";
    std::cout << "PARSE LATENCY: ";
    parse_latency.print_stats();
    
    return 0;
}
