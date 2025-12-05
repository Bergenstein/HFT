// ============================================================================
// core/cache_line.hpp - CPU Cache Line Size for Cache-Aware Data Structures
// ============================================================================
//
// 📚 EDUCATIONAL OVERVIEW
// -----------------------
// This file defines the CPU cache line size - a fundamental constant for
// building high-performance, cache-efficient data structures in HFT systems.
//
// 🎯 WHERE THIS IS USED IN THE SYSTEM:
// ------------------------------------
// - pipeline/spsc_queue.hpp: Aligns producer/consumer cursors to prevent false sharing
// - pipeline/mpmc_queue.hpp: Aligns queue metadata for multi-producer/consumer
// - core/memory_pool.hpp: Aligns memory blocks to cache line boundaries
//
// 💡 KEY HFT CONCEPTS:
// --------------------
// 1. CACHE LINE: Smallest unit of memory transfer between CPU cache and RAM
// 2. FALSE SHARING: Two threads accessing different variables in same cache line
// 3. CACHE COHERENCY: Protocol to keep caches synchronized across cores
// 4. MESI PROTOCOL: Modified/Exclusive/Shared/Invalid states
//
// ⚡ WHY 64 BYTES?
// ----------------
// Modern x86-64 CPUs (Intel, AMD) use 64-byte cache lines:
// - Intel Core i3/i5/i7/i9: 64 bytes
// - AMD Ryzen/EPYC: 64 bytes
// - ARM Cortex-A (Apple Silicon): 64 bytes (some models use 128 bytes)
//
// HISTORICAL NOTE:
// - Older CPUs (Pentium 4): 128 bytes
// - Ancient CPUs (Pentium III): 32 bytes
// - Modern standard: 64 bytes (since ~2006)
//
// 🔬 TECHNICAL DEEP DIVE: FALSE SHARING
// --------------------------------------
//
// PROBLEM SCENARIO:
// ```cpp
// struct Counter {
//     int thread1_counter;  // Bytes 0-3
//     int thread2_counter;  // Bytes 4-7
// };
// 
// // Thread 1 writes thread1_counter
// // Thread 2 writes thread2_counter
// // Both in same 64-byte cache line → DISASTER!
// ```
//
// WHAT HAPPENS:
// 1. Thread 1 writes thread1_counter
//    → Cache line in Thread 1's L1 cache marked "Modified"
//    → Other cores' copies marked "Invalid"
// 
// 2. Thread 2 tries to write thread2_counter
//    → Its cache line is "Invalid"
//    → Must fetch from Thread 1's cache (cache-to-cache transfer)
//    → 40-60ns latency (vs 0.5ns L1 hit)
// 
// 3. Thread 1 writes again
//    → Now Thread 2's cache is "Invalid"
//    → Fetch from Thread 1 again
// 
// 4. This ping-pong repeats EVERY write → 10-100x slowdown!
//
// CACHE COHERENCY TRAFFIC:
// - Without false sharing: 0-1 cache-to-cache transfers per thousand ops
// - With false sharing: 500-1000 transfers per thousand ops
// - Performance impact: 50-100x slower
//
// SOLUTION:
// ```cpp
// struct alignas(64) Counter {
//     int thread1_counter;          // Bytes 0-3
//     char padding1[60];            // Bytes 4-63 (pad to 64)
// };
// 
// struct alignas(64) Counter2 {
//     int thread2_counter;          // Bytes 64-67 (next cache line)
//     char padding2[60];            // Bytes 68-127
// };
// 
// // Now in different cache lines → no false sharing!
// ```
//
// 📊 REAL-WORLD IMPACT
// --------------------
// Production HFT system (SPSC queue, 1M msgs/sec):
// 
// WITHOUT cache line alignment:
//   - Throughput: 200K msgs/sec
//   - Latency P99: 500ns
//   - CPU: 80% (cache thrashing)
// 
// WITH cache line alignment:
//   - Throughput: 5M msgs/sec (25x faster!)
//   - Latency P99: 50ns (10x faster!)
//   - CPU: 40% (efficient cache usage)
//
// 🎓 INTERVIEW TALKING POINTS:
// ----------------------------
// Q: What is a cache line?
// A: Smallest unit of cache coherency (64 bytes on modern CPUs).
//    When you access a single byte, CPU fetches entire 64-byte line.
//
// Q: What is false sharing?
// A: Two threads accessing different variables in same cache line,
//    causing unnecessary cache synchronization (40-60ns overhead per access).
//
// Q: How to prevent false sharing?
// A: Align each thread's data to cache line boundaries using alignas(64).
//    Wastes memory (padding) but gains 10-100x speed.
//
// Q: When to use cache line alignment?
// A: For variables written by different threads:
//    - Queue cursors (producer vs consumer)
//    - Atomic counters (per-thread stats)
//    - Lock-free data structures
//
// Q: What's the cost?
// A: Memory overhead. Example: int (4 bytes) → 64 bytes with alignment.
//    For HFT: Speed > memory, so worth it for critical structures.
//
// Q: How to verify alignment?
// A: static_assert(sizeof(MyStruct) % 64 == 0);
//    static_assert(alignof(MyStruct) == 64);
//
// Q: Alternative to padding?
// A: Hardware transactional memory (HTM), but complex and limited support.
//    Padding is simple, portable, and proven effective.
//
// ============================================================================

#pragma once
#include <cstddef>

namespace core {
    /**
     * 🏗️ CACHE LINE SIZE CONSTANT
     * ===========================
     * 
     * Used for cache-aware data structure alignment.
     * 
     * VALUE: 64 bytes (standard for modern x86-64 CPUs)
     * 
     * USAGE:
     * ```cpp
     * // Align structure to cache line boundary
     * struct alignas(core::CACHE_LINE_SIZE) MyCriticalData {
     *     std::atomic<int> counter;
     *     char padding[core::CACHE_LINE_SIZE - sizeof(std::atomic<int>)];
     * };
     * 
     * // Verify alignment
     * static_assert(sizeof(MyCriticalData) == core::CACHE_LINE_SIZE);
     * static_assert(alignof(MyCriticalData) == core::CACHE_LINE_SIZE);
     * ```
     * 
     * WHY constexpr?
     * - Compile-time constant (can use in template arguments, static_assert)
     * - Zero runtime overhead
     * - Can be optimized away by compiler
     * 
     * PLATFORM VARIATIONS:
     * - x86-64 (Intel/AMD): 64 bytes
     * - ARM Cortex-A: 64 bytes (some models: 128 bytes)
     * - PowerPC: 128 bytes
     * - For portability: 64 bytes is safe minimum
     * 
     * ADVANCED: Runtime detection (if needed for ARM):
     * ```cpp
     * #ifdef __linux__
     * #include <unistd.h>
     * long cache_line_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
     * #endif
     * ```
     */
    static constexpr size_t CACHE_LINE_SIZE = 64;
}

// ============================================================================
// 📖 ADDITIONAL RESOURCES FOR LEARNING
// ============================================================================
//
// RECOMMENDED READING:
// 1. "What Every Programmer Should Know About Memory" - Ulrich Drepper
//    https://people.freebsd.org/~lstewart/articles/cpumemory.pdf
// 
// 2. "False Sharing" - Intel Developer Guide
//    https://software.intel.com/content/www/us/en/develop/articles/avoiding-and-identifying-false-sharing-among-threads.html
//
// 3. "MESI Protocol" - Wikipedia
//    https://en.wikipedia.org/wiki/MESI_protocol
//
// TOOLS FOR DETECTING FALSE SHARING:
// - Intel VTune Profiler: Shows cache line contention
// - perf (Linux): perf c2c (cache-to-cache monitoring)
// - Valgrind with DHAT: Detects cache misses
//
// BENCHMARKING:
// ```cpp
// // Measure impact of false sharing
// #include <benchmark/benchmark.h>
// 
// struct Unaligned {
//     std::atomic<int> a;
//     std::atomic<int> b;
// };
// 
// struct alignas(64) Aligned {
//     std::atomic<int> a;
//     char pad[60];
// };
// 
// void BM_FalseSharing(benchmark::State& state) {
//     Unaligned u;
//     std::thread t1([&]{ while(running) u.a++; });
//     std::thread t2([&]{ while(running) u.b++; });
//     // Measure throughput...
// }
// // Typically: Aligned is 10-100x faster!
// ```
//
// ============================================================================
