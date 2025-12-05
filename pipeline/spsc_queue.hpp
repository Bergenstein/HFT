// pipeline/spsc_queue.hpp - Lock-Free Single Producer Single Consumer Queue
#pragma once

// Include cache line size constant for proper alignment
#include "../core/cache_line.hpp"
#include <utility>  // For std::move
#include <atomic>   // For lock-free atomic operations
#include <new>      // For placement new

namespace pipeline {

using core::CACHE_LINE_SIZE;  // Typically 64 bytes on modern x86_64 CPUs

/**
 * AlignedType: Wrapper that ensures each element in the queue is cache-line aligned
 * 
 * WHY THIS MATTERS:
 * - Without alignment, multiple elements could share the same 64-byte cache line
 * - When producer updates one element, it invalidates the entire cache line
 * - Consumer on a different core sees cache miss even for unrelated elements
 * - This is called "false sharing" and can cause 10-100x slowdown
 * 
 * EXAMPLE:
 * Without alignment: [elem0][elem1][elem2][elem3] all in same 64-byte line
 * With alignment: [elem0 + padding] [elem1 + padding] each in separate line
 */
template<typename T>
struct alignas(CACHE_LINE_SIZE) AlignedType {
    T value;  // The actual data element
    // Implicit padding added by compiler to fill remainder of 64 bytes
};

/**
 * SPSCQueue: Lock-Free Ring Buffer for Single Producer Single Consumer
 * 
 * PERFORMANCE CHARACTERISTICS:
 * - Push: O(1) - typically 5-10ns on modern CPUs
 * - Pop: O(1) - typically 5-10ns
 * - No locks, mutexes, or syscalls
 * - No heap allocation in push/pop (only at construction)
 * 
 * MEMORY ORDERING EXPLAINED:
 * - memory_order_relaxed: No synchronization, just atomic read/write
 * - memory_order_acquire: Ensures all writes before this are visible
 * - memory_order_release: Ensures all writes before this happen before the release
 * 
 * WHY SINGLE PRODUCER/SINGLE CONSUMER?
 * - Simpler than MPMC (multi-producer multi-consumer)
 * - No need for expensive CAS (compare-and-swap) loops
 * - Perfect for pipeline stages: Exchange Thread → Strategy Thread
 */
template<typename T>
class SPSCQueue {
public:
    /**
     * Constructor: Allocates the ring buffer
     * 
     * @param capacity: Desired capacity (will be rounded up to next power of 2)
     * 
     * WHY POWER OF 2?
     * - Allows fast modulo using bitwise AND: (index + 1) & mask
     * - CPU can execute bitwise AND in 1 cycle, modulo takes 20-40 cycles
     * - Example: capacity=1000000 → rounds to 1048576 (2^20)
     */
    explicit SPSCQueue(size_t capacity) 
        : capacity_(next_power_of_2(capacity)),  // Round up to power of 2
          mask_(capacity_ - 1),                   // For fast modulo: x % capacity_ == x & mask_
          buffer_(new AlignedType<T>[capacity_]), // Heap-allocate the ring buffer
          head_(0),  // Consumer's read position
          tail_(0)   // Producer's write position
    {}
    
    // Destructor: Free the ring buffer
    ~SPSCQueue() { delete[] buffer_; }
    
    // Disable copy constructor and assignment (queue owns the buffer)
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    
    /**
     * try_push: Enqueue an item (copy semantics)
     * 
     * ALGORITHM:
     * 1. Load current tail position (producer owns this, so relaxed is safe)
     * 2. Calculate next tail position using bitwise AND for fast modulo
     * 3. Check if queue is full by comparing to head (acquire to see consumer's updates)
     * 4. Write the item to the buffer
     * 5. Update tail with release semantics (makes item visible to consumer)
     * 
     * MEMORY ORDERING:
     * - tail_.load(relaxed): Producer owns tail, no need for synchronization
     * - head_.load(acquire): Must see consumer's latest head update
     * - tail_.store(release): Publish the new item to consumer
     * 
     * @return true if item was pushed, false if queue was full
     */
    bool try_push(const T& item) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & mask_;  // Fast modulo using bitwise AND
        
        // Queue is full if next_tail catches up to head
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Queue full
        }
        
        // Write the item to the buffer
        buffer_[current_tail].value = item;
        
        // Publish the update to consumer (release ensures item write happens first)
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    /**
     * try_push: Enqueue an item (move semantics)
     * 
     * Same as copy version, but uses std::move to avoid copying large objects
     * Useful for objects like std::string, std::vector that are expensive to copy
     */
    bool try_push(T&& item) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & mask_;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Queue full
        }
        
        // Move the item instead of copying (transfers ownership)
        buffer_[current_tail].value = std::move(item);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    /**
     * try_pop: Dequeue an item
     * 
     * ALGORITHM:
     * 1. Load current head position (consumer owns this, so relaxed is safe)
     * 2. Check if queue is empty by comparing to tail (acquire to see producer's updates)
     * 3. Read the item from the buffer
     * 4. Update head with release semantics (makes slot available to producer)
     * 
     * MEMORY ORDERING:
     * - head_.load(relaxed): Consumer owns head, no need for synchronization
     * - tail_.load(acquire): Must see producer's latest tail update
     * - head_.store(release): Publish the freed slot to producer
     * 
     * @param item: Output parameter where dequeued item is written
     * @return true if item was popped, false if queue was empty
     */
    bool try_pop(T& item) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        
        // Queue is empty if head catches up to tail
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Queue empty
        }
        
        // Read the item from the buffer
        item = buffer_[current_head].value;
        
        // Publish the update to producer (release ensures item read happens first)
        head_.store((current_head + 1) & mask_, std::memory_order_release);
        return true;
    }
    
    /**
     * size: Get the approximate number of elements in the queue
     * 
     * NOTE: This is approximate because producer and consumer are on different cores
     * By the time this returns, the size might have changed
     * Use acquire to ensure we see the latest values
     * 
     * CALCULATION:
     * - If tail >= head: size = tail - head
     * - If tail < head (wrapped): size = (tail + capacity) - head
     * - Bitwise AND with mask_ handles both cases automatically
     */
    size_t size() const noexcept {
        const size_t current_head = head_.load(std::memory_order_acquire);
        const size_t current_tail = tail_.load(std::memory_order_acquire);
        return (current_tail - current_head) & mask_;
    }
    
    /**
     * empty: Check if the queue is empty
     * 
     * NOTE: This is a snapshot - by the time you act on it, queue state might change
     */
    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }
    
    // Get the maximum capacity of the queue
    size_t capacity() const noexcept { return capacity_; }

private:
    /**
     * is_power_of_2: Check if a number is a power of 2
     * 
     * BIT TRICK EXPLAINED:
     * - Powers of 2 have exactly one bit set: 8 = 0b1000, 16 = 0b10000
     * - n - 1 flips all bits after the single set bit: 8-1 = 0b0111
     * - n & (n-1) clears that bit, result is 0 only for powers of 2
     * 
     * EXAMPLES:
     * - 8: 0b1000 & 0b0111 = 0b0000 = 0 ✓
     * - 7: 0b0111 & 0b0110 = 0b0110 ≠ 0 ✗
     */
    static bool is_power_of_2(size_t n) noexcept {
        return n > 0 && (n & (n - 1)) == 0;
    }
    
    /**
     * next_power_of_2: Round up to the next power of 2
     * 
     * BIT TRICK EXPLAINED:
     * 1. Decrement n to handle exact powers of 2
     * 2. Propagate the highest set bit to all lower bits using OR shifts
     * 3. Add 1 to get the next power of 2
     * 
     * EXAMPLE: n = 100 (0b1100100)
     * - n--:       0b1100011
     * - |= >>1:    0b1110011
     * - |= >>2:    0b1111111
     * - |= >>4:    0b1111111
     * - n+1:       0b10000000 = 128 ✓
     */
    static size_t next_power_of_2(size_t n) noexcept {
        if (n == 0) return 1;
        n--;
        n |= n >> 1;  n |= n >> 2;  n |= n >> 4;
        n |= n >> 8;  n |= n >> 16; n |= n >> 32;
        return n + 1;
    }
    
    const size_t capacity_;  // Total capacity (power of 2)
    const size_t mask_;      // capacity_ - 1, used for fast modulo
    AlignedType<T>* buffer_; // Ring buffer (heap-allocated)
    
    // CRITICAL: head_ and tail_ on separate cache lines to prevent false sharing
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;  // Consumer's read position
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;  // Producer's write position
};

} // namespace pipeline
