#pragma once
#include <chrono>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>

class LatencyTracker {
public:
    void add_sample(long long latency_ns) {
        samples_.push_back(latency_ns);
        if (samples_.size() >= 1000) {
            print_stats();
            samples_.clear();
        }
    }

    void print_stats() {
        if (samples_.empty()) return;
        
        std::sort(samples_.begin(), samples_.end());
        size_t n = samples_.size();
        
        long long min_ns = samples_.front();
        long long max_ns = samples_.back();
        long long p50_ns = samples_[n/2];
        long long p95_ns = samples_[(n*95)/100];
        long long p99_ns = samples_[(n*99)/100];
        
        double sum = 0;
        for (auto ns : samples_) sum += ns;
        double avg_ns = sum / n;
        
        std::cout << std::fixed << std::setprecision(2)
                  << "[LATENCY] n=" << n 
                  << " | min=" << (min_ns/1000.0) << "μs"
                  << " | avg=" << (avg_ns/1000.0) << "μs"
                  << " | p50=" << (p50_ns/1000.0) << "μs"
                  << " | p95=" << (p95_ns/1000.0) << "μs"
                  << " | p99=" << (p99_ns/1000.0) << "μs"
                  << " | max=" << (max_ns/1000.0) << "μs"
                  << std::endl;
    }

private:
    std::vector<long long> samples_;
};
