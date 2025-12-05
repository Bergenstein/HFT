// run/test_lockfree_queues.cpp - Test lock-free queue performance
#include "../pipeline/spsc_queue.hpp"
#include "../pipeline/mpmc_queue.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

using namespace pipeline;

// Test data structure
struct MarketData {
    double price;
    double size;
    uint64_t sequence;
    uint64_t timestamp;
};

void test_spsc_queue() {
    std::cout << "\n=== SPSC Queue Test ===\n";
    
    const size_t NUM_ITEMS = 10000000;  // 10M items
    SPSCQueue<MarketData> queue(1048576);  // 1M capacity
    
    std::atomic<bool> done{false};
    std::atomic<uint64_t> items_consumed{0};
    
    // Consumer thread
    auto consumer = std::thread([&]() {
        MarketData data;
        while (!done.load() || queue.size() > 0) {
            if (queue.try_pop(data)) {
                items_consumed.fetch_add(1);
            }
        }
    });
    
    // Producer thread
    auto start = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < NUM_ITEMS; ++i) {
        MarketData data{42150.5 + i, 1.5, i, i};
        
        while (!queue.try_push(data)) {
            // Queue full, spin
        }
    }
    
    done.store(true);
    consumer.join();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    double throughput = (NUM_ITEMS * 1000000.0) / duration.count();
    double latency_ns = (duration.count() * 1000.0) / NUM_ITEMS;
    
    std::cout << "Items produced: " << NUM_ITEMS << "\n";
    std::cout << "Items consumed: " << items_consumed.load() << "\n";
    std::cout << "Duration: " << duration.count() << " μs\n";
    std::cout << "Throughput: " << throughput << " items/sec\n";
    std::cout << "Avg latency: " << latency_ns << " ns/item\n";
    std::cout << "Result: " << (items_consumed == NUM_ITEMS ? "PASS ✓" : "FAIL ✗") << "\n";
}

void test_mpmc_queue() {
    std::cout << "\n=== MPMC Queue Test ===\n";
    
    const size_t NUM_PRODUCERS = 4;
    const size_t NUM_CONSUMERS = 4;
    const size_t ITEMS_PER_PRODUCER = 1000000;
    const size_t TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    
    MPMCQueue<MarketData> queue(1048576);
    
    std::atomic<bool> done{false};
    std::atomic<uint64_t> items_consumed{0};
    std::vector<std::thread> threads;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Start consumers
    for (size_t i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back([&]() {
            MarketData data;
            while (!done.load() || queue.size() > 0) {
                if (queue.try_dequeue(data)) {
                    items_consumed.fetch_add(1);
                }
            }
        });
    }
    
    // Start producers
    for (size_t p = 0; p < NUM_PRODUCERS; ++p) {
        threads.emplace_back([&, p]() {
            for (size_t i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                MarketData data{42150.5 + i, 1.5, p * ITEMS_PER_PRODUCER + i, i};
                
                while (!queue.try_enqueue(data)) {
                    // Queue full, spin
                }
            }
        });
    }
    
    // Wait for producers
    for (size_t i = NUM_CONSUMERS; i < threads.size(); ++i) {
        threads[i].join();
    }
    
    done.store(true);
    
    // Wait for consumers
    for (size_t i = 0; i < NUM_CONSUMERS; ++i) {
        threads[i].join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    double throughput = (TOTAL_ITEMS * 1000000.0) / duration.count();
    double latency_ns = (duration.count() * 1000.0) / TOTAL_ITEMS;
    
    std::cout << "Producers: " << NUM_PRODUCERS << "\n";
    std::cout << "Consumers: " << NUM_CONSUMERS << "\n";
    std::cout << "Items per producer: " << ITEMS_PER_PRODUCER << "\n";
    std::cout << "Total items: " << TOTAL_ITEMS << "\n";
    std::cout << "Items consumed: " << items_consumed.load() << "\n";
    std::cout << "Duration: " << duration.count() << " μs\n";
    std::cout << "Throughput: " << throughput << " items/sec\n";
    std::cout << "Avg latency: " << latency_ns << " ns/item\n";
    std::cout << "Result: " << (items_consumed == TOTAL_ITEMS ? "PASS ✓" : "FAIL ✗") << "\n";
}

int main() {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║        LOCK-FREE QUEUE PERFORMANCE TEST                      ║
║        Testing HFT-grade data structures                     ║
╚══════════════════════════════════════════════════════════════╝
)";
    
    test_spsc_queue();
    test_mpmc_queue();
    
    std::cout << "\n✓ All lock-free queue tests completed\n\n";
    
    return 0;
}
