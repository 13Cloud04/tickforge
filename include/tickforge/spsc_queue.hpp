#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

namespace tf {

// Apple Silicon has 128-byte cache lines; x86-64 has 64. Padding to the larger
// of the two keeps the producer's and consumer's indices from false sharing
// on either.
inline constexpr std::size_t kCacheLine = 128;

// Bounded, wait-free single-producer / single-consumer ring buffer.
//
// Each side owns one atomic index and keeps a private cached copy of the
// other side's index, so the common case of push/pop touches no cache line
// that the other thread writes. The shared index is only re-read when the
// cached copy says the queue looks full (producer) or empty (consumer).
template <class T>
class SpscQueue {
public:
    // Capacity is rounded up to a power of two.
    explicit SpscQueue(std::size_t capacity) {
        std::size_t cap = 2;
        while (cap < capacity) cap <<= 1;
        mask_ = cap - 1;
        buf_ = std::make_unique<T[]>(cap);
    }

    std::size_t capacity() const { return mask_ + 1; }

    bool try_push(const T& v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ > mask_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ > mask_) return false;
        }
        buf_[t & mask_] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = buf_[h & mask_];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

private:
    std::unique_ptr<T[]> buf_;
    std::size_t mask_ = 0;

    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by producer
    std::size_t head_cache_ = 0;                            // producer-private

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by consumer
    std::size_t tail_cache_ = 0;                            // consumer-private

    char pad_[kCacheLine - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];
};

}  // namespace tf
