#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <iomanip>
#include <sstream>

namespace core {

/**
 * High-precision timestamp utilities for latency measurement
 * Uses std::chrono::steady_clock for monotonic, high-resolution timing
 */
class Timestamp {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    using Nanos = std::chrono::nanoseconds;
    using Micros = std::chrono::microseconds;
    
    // Get current timestamp
    static TimePoint now() {
        return Clock::now();
    }
    
    // Convert to nanoseconds since epoch
    static int64_t to_nanos(const TimePoint& tp) {
        return std::chrono::duration_cast<Nanos>(tp.time_since_epoch()).count();
    }
    
    // Convert to microseconds since epoch
    static int64_t to_micros(const TimePoint& tp) {
        return std::chrono::duration_cast<Micros>(tp.time_since_epoch()).count();
    }
    
    // Calculate latency in nanoseconds
    static int64_t latency_nanos(const TimePoint& start, const TimePoint& end) {
        return std::chrono::duration_cast<Nanos>(end - start).count();
    }
    
    // Calculate latency in microseconds
    static double latency_micros(const TimePoint& start, const TimePoint& end) {
        return std::chrono::duration_cast<Micros>(end - start).count();
    }
    
    // Get wall-clock time for logging
    static std::string now_iso8601() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t), "%Y-%m-%dT%H:%M:%S");
        oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }
};

/**
 * Latency statistics tracker
 * Tracks min, max, mean, p50, p95, p99 latencies
 */
class LatencyStats {
private:
    std::vector<int64_t> samples_;  // Latencies in nanoseconds
    int64_t sum_ = 0;
    int64_t min_ = INT64_MAX;
    int64_t max_ = 0;
    size_t count_ = 0;
    
public:
    void add_sample(int64_t latency_nanos) {
        samples_.push_back(latency_nanos);
        sum_ += latency_nanos;
        min_ = std::min(min_, latency_nanos);
        max_ = std::max(max_, latency_nanos);
        count_++;
    }
    
    void add_latency(const Timestamp::TimePoint& start, const Timestamp::TimePoint& end) {
        add_sample(Timestamp::latency_nanos(start, end));
    }
    
    // Get percentile (requires sorting)
    double percentile(double p) {
        if (samples_.empty()) return 0.0;
        
        auto sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        
        size_t idx = static_cast<size_t>(p * sorted.size());
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        
        return sorted[idx] / 1000.0; // Return in microseconds
    }
    
    double mean_micros() const {
        return count_ > 0 ? (sum_ / static_cast<double>(count_)) / 1000.0 : 0.0;
    }
    
    double min_micros() const {
        return min_ == INT64_MAX ? 0.0 : min_ / 1000.0;
    }
    
    double max_micros() const {
        return max_ / 1000.0;
    }
    
    size_t count() const { return count_; }
    
    void print(std::ostream& os, const std::string& label = "") const {
        if (!label.empty()) {
            os << label << ":\n";
        }
        os << "  Count: " << count_ << "\n";
        os << "  Mean:  " << std::fixed << std::setprecision(2) << mean_micros() << " μs\n";
        os << "  Min:   " << min_micros() << " μs\n";
        os << "  Max:   " << max_micros() << " μs\n";
        
        if (!samples_.empty()) {
            auto sorted = samples_;
            std::sort(sorted.begin(), sorted.end());
            os << "  P50:   " << sorted[sorted.size() / 2] / 1000.0 << " μs\n";
            os << "  P95:   " << sorted[static_cast<size_t>(sorted.size() * 0.95)] / 1000.0 << " μs\n";
            os << "  P99:   " << sorted[static_cast<size_t>(sorted.size() * 0.99)] / 1000.0 << " μs\n";
        }
    }
    
    void reset() {
        samples_.clear();
        sum_ = 0;
        min_ = INT64_MAX;
        max_ = 0;
        count_ = 0;
    }
};

/**
 * Scoped latency timer
 * Automatically measures latency from construction to destruction
 */
class ScopedTimer {
private:
    Timestamp::TimePoint start_;
    LatencyStats& stats_;
    
public:
    explicit ScopedTimer(LatencyStats& stats) 
        : start_(Timestamp::now()), stats_(stats) {}
    
    ~ScopedTimer() {
        stats_.add_latency(start_, Timestamp::now());
    }
};

} // namespace core
