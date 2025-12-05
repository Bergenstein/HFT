// ============================================================================
// core/cpu_affinity.hpp - CPU Pinning & Thread Priority for HFT
// ============================================================================
//
// 📚 EDUCATIONAL OVERVIEW
// -----------------------
// This file implements CPU core pinning (thread affinity) - a critical
// technique in HFT systems for minimizing latency variance and achieving
// deterministic performance.
//
// 🎯 WHERE THIS IS USED IN THE SYSTEM:
// ------------------------------------
// - run/production_multi_exchange*.cpp: Pin each exchange connector to core
// - run/integrated_pipeline_demo.cpp: Pin pipeline stages to cores
// - run/multi_exchange_live_pipeline.cpp: Multi-exchange core assignment
// - pipeline/hft_pipeline.hpp: Pin market data, strategy, execution threads
//
// 💡 KEY HFT CONCEPTS:
// --------------------
// 1. CPU AFFINITY: Bind thread to specific core (no migration)
// 2. CONTEXT SWITCHING: OS moving thread between cores (10-50μs penalty)
// 3. CACHE LOCALITY: Each core has L1/L2 cache; migration = cache miss
// 4. NUMA: Non-Uniform Memory Access (memory closer to some cores)
//
// ⚡ PERFORMANCE IMPACT:
// ---------------------
// WITHOUT pinning:
// - Thread migrates between cores every 10-100ms
// - Each migration: 10-50μs latency spike + cold cache (50-200ns/access)
// - Latency variance: ±50μs (unacceptable for HFT)
//
// WITH pinning:
// - Thread stays on one core entire lifetime
// - Hot cache (L1: 0.5ns, L2: 7ns, L3: 20ns)
// - Latency variance: ±100ns (acceptable)
//
// 📊 REAL-WORLD EXAMPLE:
// ----------------------
// Market data thread (unpinned):
//   - P50: 5μs, P99: 150μs (context switches)
// Market data thread (pinned to Core 0):
//   - P50: 2μs, P99: 8μs (stable, predictable)
//
// 🔬 TECHNICAL DETAILS:
// ---------------------
// Linux (pthread_setaffinity_np):
//   - Hard pinning: Thread ONLY runs on specified core
//   - Survives process migration, scheduler decisions
//   - Requires CAP_SYS_NICE capability (or run as root)
//
// macOS (thread_affinity_policy):
//   - Soft hints: Scheduler prefers specified core but can override
//   - macOS philosophy: Don't let user break scheduler
//   - Less effective than Linux but still helps
//
// CORE NUMBERING:
//   - Cores numbered 0 to N-1 (N = hardware_concurrency())
//   - On SMT/Hyperthreading: logical cores (2x physical cores)
//   - Example: 8-core CPU with HT → 16 logical cores (0-15)
//   - Prefer: Even cores (0,2,4...) = physical, Odd (1,3,5...) = HT siblings
//
// 🎓 INTERVIEW TALKING POINTS:
// ----------------------------
// Q: Why pin threads to cores?
// A: Eliminate context switch latency (10-50μs) and maintain hot cache.
//    HFT needs <10μs P99 latency; unpinned threads have 100+μs spikes.
//
// Q: Which threads should be pinned?
// A: Critical path: market data, strategy, order routing
//    Non-critical: logging, monitoring, storage (can share cores)
//
// Q: What about hyperthreading?
// A: Use physical cores for critical threads (even numbers).
//    HT siblings share L1/L2 cache → can cause interference.
//
// Q: NUMA considerations?
// A: Pin threads to cores on same NUMA node as network card.
//    Cross-NUMA latency: +50-100ns per memory access.
//
// Q: How to verify pinning works?
// A: Use `taskset -c -p <pid>` (Linux) or Activity Monitor (macOS).
//    Check /proc/<pid>/status (Linux) for Cpus_allowed_list.
//
// ============================================================================

#pragma once
#include <thread>
#include <iostream>

// Platform-specific headers for CPU affinity
#ifdef __APPLE__
#include <mach/thread_policy.h>  // thread_affinity_policy
#include <mach/thread_act.h>     // thread_policy_set
#include <pthread.h>              // pthread_mach_thread_np
#elif defined(__linux__)
#include <sched.h>                // CPU_SET, sched_setaffinity
#include <pthread.h>              // pthread_setaffinity_np
#endif

namespace core {

/**
 * 📌 PIN THREAD TO CORE - Eliminate Context Switching
 * ===================================================
 * 
 * Binds the calling thread to a specific CPU core.
 * 
 * PARAMETERS:
 * - core_id: CPU core number (0 to num_cores-1)
 * 
 * RETURNS:
 * - true: Successfully pinned
 * - false: Failed (wrong core ID, insufficient permissions, unsupported platform)
 * 
 * USAGE EXAMPLE:
 * ```cpp
 * // Market data thread - pin to Core 0
 * std::thread md_thread([]{
 *     core::pin_thread_to_core(0);
 *     // ... market data processing
 * });
 * 
 * // Strategy thread - pin to Core 1
 * std::thread strat_thread([]{
 *     core::pin_thread_to_core(1);
 *     // ... strategy execution
 * });
 * ```
 * 
 * PLATFORM DIFFERENCES:
 * 
 * LINUX (pthread_setaffinity_np):
 * - Hard pinning: OS guarantees thread only runs on specified core
 * - Persists across sleep/wake cycles
 * - Requires no special privileges for own threads
 * - Use cpu_set_t to specify multiple cores (we pin to single core)
 * 
 * MACOS (thread_affinity_policy):
 * - Soft hints: Scheduler *prefers* core but can override
 * - Less strict than Linux (Apple's design philosophy)
 * - Still provides significant benefit (reduces migrations)
 * - Cannot achieve same determinism as Linux
 * 
 * COMMON PITFALLS:
 * - Pinning to core >= num_cores: Fails silently on some platforms
 * - Pinning all threads to same core: No parallelism!
 * - Not leaving cores for OS: Can starve system processes
 * 
 * BEST PRACTICES:
 * - Reserve Core 0 for most critical thread (market data)
 * - Leave at least 1-2 cores free for OS
 * - On HT CPUs: Use even-numbered cores (physical cores first)
 * - Verify with `taskset -c -p <pid>` (Linux)
 */
inline bool pin_thread_to_core(int core_id) {
#ifdef __linux__
    // Create CPU set (bitmask of allowed cores)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);        // Clear all bits
    CPU_SET(core_id, &cpuset); // Set bit for our core
    
    // Get current thread handle
    pthread_t current_thread = pthread_self();
    
    // Set affinity: Restrict this thread to cpuset
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    
    if (result != 0) {
        std::cerr << "[CPU] Failed to pin thread to core " << core_id << "\n";
        return false;
    }
    
    std::cout << "[CPU] ✓ Pinned thread to core " << core_id << "\n";
    return true;
    
#elif defined(__APPLE__)
    // macOS doesn't support hard CPU pinning, but we can set affinity hints
    // This tells the scheduler to *prefer* keeping thread on this core
    thread_affinity_policy_data_t policy = { core_id };
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    
    kern_return_t result = thread_policy_set(
        mach_thread,
        THREAD_AFFINITY_POLICY,      // Policy type
        (thread_policy_t)&policy,     // Policy data
        THREAD_AFFINITY_POLICY_COUNT  // Policy count
    );
    
    if (result != KERN_SUCCESS) {
        std::cerr << "[CPU] Failed to set thread affinity to core " << core_id << "\n";
        return false;
    }
    
    std::cout << "[CPU] ✓ Set thread affinity hint for core " << core_id << " (macOS soft hint)\n";
    return true;
    
#else
    std::cerr << "[CPU] ⚠️  CPU pinning not supported on this platform\n";
    return false;
#endif
}

/**
 * ⚡ SET REALTIME PRIORITY - Preempt Non-Critical Threads
 * =======================================================
 * 
 * Elevates thread to real-time scheduling class (SCHED_FIFO or SCHED_RR).
 * 
 * REAL-TIME SCHEDULING:
 * - Regular threads: SCHED_OTHER (time-sharing, fair scheduler)
 * - Realtime threads: SCHED_FIFO (first-in-first-out, no time slicing)
 *                     SCHED_RR (round-robin, time-sliced realtime)
 * 
 * WHAT THIS DOES:
 * - Thread runs until it blocks or yields (no preemption by other threads)
 * - Higher priority than ALL non-realtime threads
 * - Can starve other processes (use carefully!)
 * 
 * PRIORITY LEVELS:
 * - Linux: 1-99 (99 = highest), we use max (sched_get_priority_max)
 * - macOS: 0-63 (63 = highest)
 * 
 * REQUIREMENTS:
 * - Linux: CAP_SYS_NICE capability OR run as root
 * - macOS: Generally works without special privileges
 * - Fails gracefully if insufficient permissions
 * 
 * USAGE EXAMPLE:
 * ```cpp
 * std::thread critical_thread([]{
 *     if (core::set_realtime_priority()) {
 *         core::pin_thread_to_core(0);
 *         // ... ultra-low-latency processing
 *     }
 * });
 * ```
 * 
 * ⚠️  DANGERS:
 * - Can lock up system if thread doesn't yield
 * - Can starve critical OS processes (networking, storage)
 * - Use ONLY for critical HFT threads
 * - Add sleep/yield to prevent 100% CPU usage
 * 
 * BEST PRACTICES:
 * - Only 1-3 threads should be realtime (market data, strategy)
 * - Always include blocking calls (network I/O, queue waits)
 * - Monitor CPU usage; if 100% sustained, add yields
 * - In production: Run in Docker/cgroup with CPU limits
 */
inline bool set_realtime_priority() {
#ifdef __linux__
    struct sched_param param;
    // Get maximum priority for FIFO scheduling (typically 99)
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    
    // Set scheduling policy to FIFO with max priority
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        std::cerr << "[CPU] ⚠️  Failed to set real-time priority (need sudo or CAP_SYS_NICE capability)\n";
        std::cerr << "[CPU] Run with: sudo setcap cap_sys_nice=eip <your_binary>\n";
        return false;
    }
    
    std::cout << "[CPU] ✓ Set real-time priority: SCHED_FIFO with priority " 
              << param.sched_priority << "\n";
    return true;
    
#elif defined(__APPLE__)
    // macOS real-time scheduling (round-robin)
    struct sched_param param;
    param.sched_priority = 63;  // Max priority on macOS
    
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &param) != 0) {
        std::cerr << "[CPU] ⚠️  Failed to set real-time priority\n";
        return false;
    }
    
    std::cout << "[CPU] ✓ Set real-time priority: SCHED_RR with priority 63\n";
    return true;
    
#else
    return false;
#endif
}

/**
 * 🔥 DISABLE CPU FREQUENCY SCALING - Lock Core to Max Frequency
 * =============================================================
 * 
 * Modern CPUs dynamically adjust frequency (power saving):
 * - Idle: 800 MHz
 * - Light load: 1.5 GHz
 * - Heavy load: 3.5 GHz (turbo boost)
 * 
 * PROBLEM FOR HFT:
 * - Frequency changes take 50-500μs (latency spike!)
 * - First event after idle: slow (low frequency)
 * - Unpredictable latency variance
 * 
 * SOLUTION:
 * - Lock CPU to "performance" governor (always max frequency)
 * - Trades power consumption for latency consistency
 * 
 * GOVERNORS (Linux):
 * - powersave: Always lowest frequency
 * - ondemand: Scales with load (default on most systems)
 * - performance: Always max frequency ← We want this!
 * - schedutil: Scheduler-based scaling
 * 
 * REQUIREMENTS:
 * - Linux only (macOS doesn't expose this control)
 * - Root privileges (need to write to /sys/devices/...)
 * - Or CAP_SYS_ADMIN capability
 * 
 * USAGE:
 * ```cpp
 * // At startup, for each core used by HFT threads
 * core::disable_cpu_frequency_scaling(0);
 * core::disable_cpu_frequency_scaling(1);
 * ```
 * 
 * VERIFICATION (Linux):
 * ```bash
 * cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
 * # Should show: performance
 * 
 * watch -n 1 cat /proc/cpuinfo | grep MHz
 * # Should show constant max frequency
 * ```
 * 
 * ⚠️  TRADE-OFFS:
 * - Pro: Latency variance drops from ±50μs to ±1μs
 * - Con: Higher power consumption (~20-50W more)
 * - Con: Higher heat (may need better cooling)
 * - Pro: Worth it for HFT (latency > power)
 */
inline void disable_cpu_frequency_scaling(int core_id) {
#ifdef __linux__
    // Write "performance" to scaling_governor file
    // Path: /sys/devices/system/cpu/cpu<N>/cpufreq/scaling_governor
    std::string cmd = "echo performance > /sys/devices/system/cpu/cpu" + 
                     std::to_string(core_id) + "/cpufreq/scaling_governor 2>/dev/null";
    int result = system(cmd.c_str());
    
    if (result == 0) {
        std::cout << "[CPU] ✓ Disabled frequency scaling for core " << core_id 
                  << " (locked to max frequency)\n";
    } else {
        std::cerr << "[CPU] ⚠️  Failed to disable frequency scaling for core " << core_id 
                  << " (need root or run: sudo cpupower frequency-set -g performance)\n";
    }
#else
    // macOS and other platforms don't expose frequency scaling control
    // macOS manages this automatically and doesn't allow user override
    (void)core_id; // Suppress unused parameter warning
#endif
}

/**
 * 🔢 GET NUMBER OF CPU CORES
 * ==========================
 * 
 * Returns number of logical CPU cores (including hyperthreading).
 * 
 * EXAMPLES:
 * - 4-core CPU without HT: returns 4
 * - 4-core CPU with HT: returns 8 (4 physical × 2 threads/core)
 * - 8-core CPU with HT: returns 16
 * 
 * USAGE:
 * ```cpp
 * int num_cores = core::get_num_cores();
 * std::cout << "System has " << num_cores << " logical cores\n";
 * ```
 */
inline int get_num_cores() {
    return std::thread::hardware_concurrency();
}

/**
 * 🎯 CORE ASSIGNMENT - Recommended Core Layout for HFT System
 * ===========================================================
 * 
 * Defines optimal core allocation for different HFT components.
 * 
 * ARCHITECTURE:
 * 
 * Core 0: MARKET DATA INGESTION
 *   - Highest priority (closest to NIC interrupt)
 *   - WebSocket receive, proto decode, L2 updates
 *   - Should be on same NUMA node as network card
 * 
 * Core 1: STRATEGY EXECUTION
 *   - Reads from market data queue
 *   - Computes signals, makes trading decisions
 *   - Generates orders
 * 
 * Core 2: ORDER ROUTING
 *   - Sends orders to exchange
 *   - Handles order acknowledgments
 *   - Critical for execution latency
 * 
 * Core 3: RISK MANAGEMENT
 *   - Pre-trade risk checks (limits, margin)
 *   - Position tracking
 *   - P&L calculation
 * 
 * Core 4: STORAGE & PERSISTENCE
 *   - Write trade logs, market data snapshots
 *   - Non-critical (can tolerate latency)
 * 
 * Core 5: MONITORING & LOGGING
 *   - Metrics collection, health checks
 *   - Prometheus/Grafana integration
 *   - Lowest priority
 * 
 * NUMA AWARENESS:
 * On multi-socket systems:
 * - Pin cores 0-2 (critical path) to same socket as NIC
 * - Check NUMA layout: `numactl --hardware`
 * - Avoid cross-socket memory access (+50-100ns)
 * 
 * HYPERTHREADING:
 * - Physical cores: 0, 2, 4, 6, 8, 10...
 * - HT siblings:    1, 3, 5, 7, 9, 11...
 * - Use physical cores for critical threads
 * - HT siblings share L1/L2 cache → can interfere
 */
struct CoreAssignment {
    int market_data_core;      // Core 0: Market data ingestion (highest priority)
    int strategy_core;         // Core 1: Strategy execution
    int order_routing_core;    // Core 2: Order management & routing
    int risk_mgmt_core;        // Core 3: Risk checks & limits
    int storage_core;          // Core 4: Database writes & persistence
    int monitoring_core;       // Core 5: Monitoring, logging, metrics
    
    /**
     * Get default core assignment based on available cores.
     * 
     * Adapts to system size:
     * - 1 core: All on core 0 (not recommended for production!)
     * - 2 cores: Critical on 0, rest on 1
     * - 4+ cores: Optimal layout as described above
     * - 8+ cores: Can dedicate more cores to parallel strategies
     */
    static CoreAssignment get_default() {
        int num_cores = get_num_cores();
        CoreAssignment ca;
        
        // Critical threads get first cores
        ca.market_data_core = 0;
        ca.strategy_core = std::min(1, num_cores - 1);
        ca.order_routing_core = std::min(2, num_cores - 1);
        ca.risk_mgmt_core = std::min(3, num_cores - 1);
        
        // Non-critical can share cores if limited
        ca.storage_core = std::min(4, num_cores - 1);
        ca.monitoring_core = std::min(5, num_cores - 1);
        
        return ca;
    }
    
    /**
     * Print core assignment (for logging/debugging)
     */
    void print() const {
        std::cout << "[CPU] 🎯 Core Assignment Plan:\n"
                  << "  ⚡ Market Data:   Core " << market_data_core << " (Critical)\n"
                  << "  🧠 Strategy:      Core " << strategy_core << " (Critical)\n"
                  << "  📤 Order Routing: Core " << order_routing_core << " (Critical)\n"
                  << "  🛡️  Risk Mgmt:     Core " << risk_mgmt_core << "\n"
                  << "  💾 Storage:       Core " << storage_core << "\n"
                  << "  📊 Monitoring:    Core " << monitoring_core << "\n";
    }
};

} // namespace core

// ============================================================================
// 🎓 INTERVIEW PREPARATION NOTES
// ============================================================================
//
// KEY TALKING POINTS:
//
// 1. WHY CPU PINNING?
//    Without: Context switches cost 10-50μs, cold cache adds 50-200ns/access
//    With: Stable latency, hot cache, predictable performance
//
// 2. PINNING VS PRIORITY?
//    - Pinning: Prevents core migration (spatial locality)
//    - Priority: Prevents preemption (temporal priority)
//    - Use BOTH for critical HFT threads
//
// 3. HOW MANY THREADS TO PIN?
//    - Minimum: Market data thread (core 0)
//    - Typical: Market data, strategy, order routing (cores 0-2)
//    - Leave 1-2 cores for OS (networking, interrupts)
//
// 4. HYPERTHREADING IMPACT?
//    - HT siblings share L1/L2 cache (32KB/256KB)
//    - Can cause 10-20% slowdown if both siblings busy
//    - Use physical cores for critical threads (even numbers)
//
// 5. NUMA CONSIDERATIONS?
//    - Multi-socket systems: Memory distributed across sockets
//    - Local memory: ~80ns latency
//    - Remote memory: ~130ns latency
//    - Pin threads to same socket as NIC for lowest latency
//
// 6. FREQUENCY SCALING?
//    - Dynamic frequency changes: 50-500μs latency spikes
//    - "performance" governor: Locks to max frequency
//    - Trade-off: +20-50W power for -50μs latency variance
//
// 7. REALTIME SCHEDULING RISKS?
//    - Can starve OS processes (bad!)
//    - Can lock up system if thread doesn't yield
//    - Use only for 1-3 most critical threads
//    - Always include blocking calls (network I/O, queue waits)
//
// 8. VERIFICATION?
//    Linux:
//      - taskset -c -p <pid>  # Check affinity
//      - cat /proc/<pid>/status | grep Cpus_allowed_list
//      - cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
//    
//    Runtime monitoring:
//      - perf stat -e context-switches,cpu-migrations <program>
//      - Should see near-zero cpu-migrations for pinned threads
//
// 9. BENCHMARKS?
//    Production HFT system (100K msgs/sec):
//    - Unpinned: P50=5μs, P99=150μs (context switches)
//    - Pinned: P50=2μs, P99=8μs (stable)
//    - Pinned+RT: P50=1.5μs, P99=5μs (optimal)
//
// ============================================================================
