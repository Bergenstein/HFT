#include <iostream>
#include <fstream>
#include <chrono>
#include <nlohmann/json.hpp>
#include "../core/latency_tracker.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file>\n";
        return 1;
    }
    
    LatencyTracker tracker;
    std::ifstream ifs(argv[1]);
    std::string line;
    int count = 0;
    
    std::cout << "Testing PURE json::parse() latency\n\n";
    
    while (std::getline(ifs, line) && count < 10000) {
        if (line.empty()) continue;
        count++;
        
        auto t0 = std::chrono::high_resolution_clock::now();
        json j = json::parse(line, nullptr, false);
        auto t1 = std::chrono::high_resolution_clock::now();
        
        long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        tracker.add_sample(ns);
    }
    
    std::cout << "Processed " << count << " lines\n";
    std::cout << "PURE JSON PARSE: ";
    tracker.print_stats();
    
    return 0;
}
