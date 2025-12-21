//==============================================================================
// System Pipeline Test
// Tests ONLY system components (no strategy logic)
//==============================================================================

#include "core/order_book.hpp"
#include "core/timestamp.hpp"
#include "pipeline/spsc_queue.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>

using pipeline::SPSCQueue;
using core::OrderBook;
using core::Timestamp;

int main() {
    std::cout << "==========================================================\n";
    std::cout << "  HFT SYSTEM PIPELINE TEST\n";
    std::cout << "==========================================================\n\n";
    
    int passed = 0;
    int total = 0;
    
    // Test 1: Order Book
    std::cout << "[Test 1] Order Book Construction... ";
    total++;
    try {
        OrderBook book;
        book.set_level(true, 50000.0, 1.5);   // bid
        book.set_level(false, 50100.0, 2.0);  // ask
        
        auto bid = book.best_bid();
        auto ask = book.best_ask();
        
        if (bid && ask && bid->first == 50000.0 && ask->first == 50100.0) {
            std::cout << "✓ PASS\n";
            passed++;
        } else {
            std::cout << "✗ FAIL\n";
        }
    } catch (const std::exception& e) {
        std::cout << "✗ FAIL (" << e.what() << ")\n";
    }
    
    // Test 2: Timestamp
    std::cout << "[Test 2] Timestamp Generation... ";
    total++;
    try {
        auto ts1 = Timestamp::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto ts2 = Timestamp::now();
        
        auto diff_ns = Timestamp::latency_nanos(ts1, ts2);
        
        if (diff_ns >= 10000000) {
            std::cout << "✓ PASS\n";
            passed++;
        } else {
            std::cout << "✗ FAIL\n";
        }
    } catch (const std::exception& e) {
        std::cout << "✗ FAIL (" << e.what() << ")\n";
    }
    
    // Test 3: SPSC Queue
    std::cout << "[Test 3] SPSC Queue Performance... ";
    total++;
    try {
        SPSCQueue<int> queue(1024);
        
        std::thread producer([&queue]() {
            for (int i = 0; i < 1000; ++i) {
                while (!queue.try_push(i)) {
                    std::this_thread::yield();
                }
            }
        });
        
        int consumed = 0;
        std::thread consumer([&queue, &consumed]() {
            int value;
            while (consumed < 1000) {
                if (queue.try_pop(value)) {
                    consumed++;
                }
            }
        });
        
        producer.join();
        consumer.join();
        
        if (consumed == 1000) {
            std::cout << "✓ PASS\n";
            passed++;
        } else {
            std::cout << "✗ FAIL\n";
        }
    } catch (const std::exception& e) {
        std::cout << "✗ FAIL (" << e.what() << ")\n";
    }
    
    // Test 4: Multi-threading
    std::cout << "[Test 4] Multi-threaded Data Flow... ";
    total++;
    try {
        std::atomic<uint64_t> counter{0};
        const int NUM_THREADS = 4;
        const int ITERATIONS = 10000;
        
        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back([&counter]() {
                for (int j = 0; j < ITERATIONS; ++j) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        if (counter.load() == NUM_THREADS * ITERATIONS) {
            std::cout << "✓ PASS\n";
            passed++;
        } else {
            std::cout << "✗ FAIL\n";
        }
    } catch (const std::exception& e) {
        std::cout << "✗ FAIL (" << e.what() << ")\n";
    }
    
    std::cout << "\n==========================================================\n";
    std::cout << "  TEST RESULTS\n";
    std::cout << "==========================================================\n";
    std::cout << "Passed: " << passed << "/" << total << "\n";
    std::cout << "Failed: " << (total - passed) << "\n";
    
    if (passed == total) {
        std::cout << "\n✅ All system tests passed!\n\n";
        return 0;
    } else {
        std::cout << "\n❌ Some tests failed\n\n";
        return 1;
    }
}
