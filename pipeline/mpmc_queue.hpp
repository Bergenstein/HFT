// pipeline/mpmc_queue.hpp - Lock-Free Multi-Producer Multi-Consumer Queue
#pragma once

// Include cache line size constant for proper alignment
#include "../core/cache_line.hpp"
#include <utility>  // For std::move
#include <atomic>   // For lock-free atomic operations
#include <new>      // For placement new

namespace pipeline {

using core::CACHE_LINE_SIZE;  // Typically 64 bytes on x86_64

/**
 * MPMCQueue: Lock-Free Bounded Queue for Multiple Producers and Multiple Consumers
 * 
 * ALGORITHM: Based on Dmitry Vyukov's bounded MPMC queue
 * Paper: "Bounded MPMC queue" (2010)
 * 
 * KEY DIFFERENCES FROM SPSC:
 * - Uses sequence numbers to track cell availability
 * - Compare-and-swap (CAS) loops to handle contention
 * - Multiple threads can enqueue/dequeue simultaneously
 * 
 * PERFORMANCE:
 * - Enqueue: ~20-50ns (vs ~5ns for SPSC) - slower due to CAS retries
 * - Dequeue: ~20-50ns
 * - Under contention: can degrade to ~100-200ns
 * 
 * USE CASES:
 * - Fan-out: One producer → Multiple consumer threads
 * - Fan-in: Multiple producers → One consumer thread
 * - Work-stealing schedulers
 * 
 * SEQUENCE NUMBER PROTOCOL:
 * - seq = pos: Cell is ready for enqueue
 * - seq = pos + 1: Cell has data, ready for dequeue
 * - seq = pos + capacity: Cell was dequeued, wrapping around
 */
template<typename T>
class MPMCQueue {
private:
    /**
     * Cell: Storage unit in the ring buffer
     * 
     * STRUCTURE:
     * - sequence: Atomic counter to track cell state
     * - data: The actual data element
     * 
     * SEQUENCE NUMBER STATES:
     * Let pos = enqueue_pos or dequeue_pos
     * 
     * seq == pos:          Cell is empty, ready for enqueue
     * seq == pos + 1:      Cell has data, ready for dequeue
     * seq == pos + N:      Cell is N operations ahead (wrapped around)
     * seq < pos:           Cell is behind, queue might be full
     */
    struct Cell {
        std::atomic<size_t> sequence;  // Tracks cell availability state
        T data;                         // The actual data stored in this cell
    };
    
public:
    /**
     * Constructor: Allocates and initializes the ring buffer
     * 
     * @param capacity: Desired capacity (rounded up to power of 2)
     * 
     * INITIALIZATION:
     * - Allocate buffer
     * - Set each cell's sequence to its index (0, 1, 2, ...)
     * - This marks all cells as ready for enqueue
     */
    explicit MPMCQueue(size_t capacity)
        : capacity_(round_up_to_power_of_2(capacity)),  // Must be power of 2
          mask_(capacity_ - 1),                          // For fast modulo
          buffer_(new Cell[capacity_]),                  // Allocate ring buffer
          enqueue_pos_(0),  // Global enqueue position counter
          dequeue_pos_(0)   // Global dequeue position counter
    {
        // Initialize each cell's sequence number to its index
        // This indicates that cell[i] is ready to accept enqueue at position i
        for (size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
    
    // Destructor: Free the ring buffer
    ~MPMCQueue() { delete[] buffer_; }
    
    // Disable copy (queue owns the buffer)
    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    
    /**
     * try_enqueue: Enqueue an item (copy semantics)
     * 
     * ALGORITHM:
     * 1. Load current global enqueue position
     * 2. Loop until we successfully claim a cell:
     *    a. Calculate cell index using fast modulo (pos & mask_)
     *    b. Load cell's sequence number with acquire semantics
     *    c. Calculate difference: dif = seq - pos
     *    d. If dif == 0: Cell is ready, try to claim it with CAS
     *    e. If dif < 0: Cell is behind (queue full), give up
     *    f. If dif > 0: Cell is ahead (someone else took it), reload pos
     * 3. Write data to claimed cell
     * 4. Update sequence to pos + 1 (marks cell as having data)
     * 
     * WHY THE LOOP?
     * - Multiple threads compete for the same cell
     * - CAS (compare_exchange_weak) fails if another thread wins
     * - We retry with updated position
     * 
     * MEMORY ORDERING:
     * - enqueue_pos_.load(relaxed): Just need atomic read, no sync needed
     * - sequence.load(acquire): Must see dequeue operations
     * - compare_exchange_weak(relaxed): No need for sequential consistency
     * - sequence.store(release): Makes data visible to dequeuers
     * 
     * @param data: Item to enqueue
     * @return true if enqueued, false if queue is full
     */
    bool try_enqueue(const T& data) noexcept {
        Cell* cell;  // Pointer to the cell we're trying to enqueue into
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);  // Current global position
        
        for (;;) {  // Retry loop - keep trying until success or failure
            cell = &buffer_[pos & mask_];  // Calculate cell index using fast modulo
            size_t seq = cell->sequence.load(std::memory_order_acquire);  // Read cell state
            intptr_t dif = (intptr_t)seq - (intptr_t)pos;  // Calculate difference (signed)
            
            if (dif == 0) {
                // Cell is ready for enqueue at position 'pos'
                // Try to claim it by incrementing global enqueue_pos
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, 
                                                       std::memory_order_relaxed)) {
                    break;  // Successfully claimed this cell
                }
                // CAS failed - another thread claimed it, loop again with new pos
            } else if (dif < 0) {
                // Cell sequence is behind position - queue is full
                // Example: seq=5, pos=10 → dif=-5 → queue full
                return false;
            } else {
                // Cell sequence is ahead - another thread already claimed it
                // Reload the global position and try again
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        
        // We've successfully claimed this cell, now write the data
        cell->data = data;
        
        // Mark cell as having data (pos + 1) with release semantics
        // This makes the data visible to dequeue operations
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }
    
    /**
     * try_enqueue: Enqueue an item (move semantics)
     * 
     * Same algorithm as copy version, but uses std::move for efficiency
     */
    bool try_enqueue(T&& data) noexcept {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)pos;
            
            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, 
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        
        // Move the data instead of copying
        cell->data = std::move(data);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }
    
    /**
     * try_dequeue: Dequeue an item
     * 
     * ALGORITHM:
     * 1. Load current global dequeue position
     * 2. Loop until we successfully claim a cell:
     *    a. Calculate cell index
     *    b. Load cell's sequence number
     *    c. Calculate difference: dif = seq - (pos + 1)
     *    d. If dif == 0: Cell has data, try to claim it
     *    e. If dif < 0: Cell is empty, give up
     *    f. If dif > 0: Cell is ahead, reload pos
     * 3. Read data from claimed cell
     * 4. Update sequence to pos + capacity (marks cell as available for reuse)
     * 
     * WHY pos + 1 IN COMPARISON?
     * - Enqueue sets seq = pos + 1 when data is written
     * - Dequeue checks if seq == pos + 1 (data is ready)
     * 
     * WHY pos + mask_ + 1 FOR NEW SEQUENCE?
     * - mask_ + 1 == capacity_ (since capacity is power of 2)
     * - This marks the cell as available for the next cycle
     * - Example: capacity=256, pos=10 → new_seq = 10 + 256 = 266
     * - When enqueue_pos wraps to 266, this cell becomes available again
     * 
     * @param data: Output parameter where dequeued item is written
     * @return true if dequeued, false if queue is empty
     */
    bool try_dequeue(T& data) noexcept {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);  // Check if seq == pos + 1
            
            if (dif == 0) {
                // Cell has data ready for dequeue
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, 
                                                       std::memory_order_relaxed)) {
                    break;  // Successfully claimed this cell
                }
            } else if (dif < 0) {
                // Cell doesn't have data yet - queue is empty
                return false;
            } else {
                // Cell is ahead - reload position
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        
        // Read the data from the cell (move to avoid copying)
        data = std::move(cell->data);
        
        // Mark cell as available for next cycle
        // pos + mask_ + 1 == pos + capacity_
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }
    
    /**
     * size: Get approximate number of elements
     * 
     * NOTE: This is inherently racy in a lock-free queue
     * - enqueue_pos and dequeue_pos are read separately
     * - By the time we return, the size might have changed
     * - Useful for monitoring, not for logic decisions
     */
    size_t size() const noexcept {
        size_t enq = enqueue_pos_.load(std::memory_order_acquire);
        size_t deq = dequeue_pos_.load(std::memory_order_acquire);
        return enq - deq;  // Works correctly even with overflow
    }
    
    // Get the maximum capacity
    size_t capacity() const noexcept { return capacity_; }

private:
    /**
     * round_up_to_power_of_2: Same bit trick as SPSC queue
     * 
     * See SPSC queue comments for detailed explanation
     */
    static size_t round_up_to_power_of_2(size_t n) noexcept {
        if (n == 0) return 1;
        n--;
        n |= n >> 1; n |= n >> 2; n |= n >> 4;
        n |= n >> 8; n |= n >> 16; n |= n >> 32;
        return n + 1;
    }
    
    const size_t capacity_;  // Total capacity (power of 2)
    const size_t mask_;      // capacity_ - 1, for fast modulo
    Cell* buffer_;           // Ring buffer of cells
    
    // CRITICAL: Separate cache lines to prevent false sharing
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_;  // Global enqueue position
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeue_pos_;  // Global dequeue position
};

} // namespace pipeline
