// pipeline/queue_manager.hpp
#pragma once
#include "normalized_data.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace pipeline {

template<typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity = 100000) 
        : capacity_(capacity), head_(0), tail_(0) {
        buffer_.resize(capacity);
    }
    
    bool try_push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % capacity_;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // Queue full
        }
        
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    bool try_pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue empty
        }
        
        item = buffer_[current_head];
        head_.store((current_head + 1) % capacity_, std::memory_order_release);
        return true;
    }
    
    size_t size() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (t >= h) ? (t - h) : (capacity_ - h + t);
    }
    
    bool empty() const { return size() == 0; }

private:
    std::vector<T> buffer_;
    size_t capacity_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
};

// Multi-producer, multi-consumer blocking queue
template<typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t max_size = 100000) : max_size_(max_size) {}
    
    void push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this]() { return queue_.size() < max_size_; });
        queue_.push(item);
        not_empty_.notify_one();
    }
    
    bool try_push(const T& item, int timeout_ms = 0) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [this]() { return queue_.size() < max_size_; })) {
            return false;
        }
        queue_.push(item);
        not_empty_.notify_one();
        return true;
    }
    
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this]() { return !queue_.empty(); });
        T item = queue_.front();
        queue_.pop();
        not_full_.notify_one();
        return item;
    }
    
    bool try_pop(T& item, int timeout_ms = 0) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this]() { return !queue_.empty(); })) {
            return false;
        }
        item = queue_.front();
        queue_.pop();
        not_full_.notify_one();
        return true;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    size_t max_size_;
};

// Queue manager for different data types
class QueueManager {
public:
    LockFreeQueue<NormalizedQuote> quote_queue{100000};
    LockFreeQueue<NormalizedTrade> trade_queue{100000};
    BlockingQueue<OHLCVBar> ohlcv_queue{10000};
    
    // Statistics
    std::atomic<uint64_t> quotes_received{0};
    std::atomic<uint64_t> trades_received{0};
    std::atomic<uint64_t> quotes_dropped{0};
    std::atomic<uint64_t> trades_dropped{0};
    
    bool push_quote(const NormalizedQuote& quote) {
        quotes_received++;
        if (!quote_queue.try_push(quote)) {
            quotes_dropped++;
            return false;
        }
        return true;
    }
    
    bool push_trade(const NormalizedTrade& trade) {
        trades_received++;
        if (!trade_queue.try_push(trade)) {
            trades_dropped++;
            return false;
        }
        return true;
    }
};

} // namespace pipeline
