#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.hpp"

namespace tf {

// Open-addressing hash map from OrderId to a 32-bit pool index.
//
// Linear probing keeps a lookup inside one or two cache lines, and deletion
// uses backward shifting instead of tombstones, so a book that churns through
// millions of add/cancel pairs never degrades or needs a cleanup rehash.
class IdMap {
public:
    static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

    explicit IdMap(std::size_t expected = 1024) {
        std::size_t cap = 16;
        while (cap < expected * 2) cap <<= 1;
        init(cap);
    }

    std::size_t size() const { return size_; }

    std::uint32_t find(OrderId key) const {
        for (std::size_t i = ideal(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.val == kEmpty) return kEmpty;
            if (s.key == key) return s.val;
        }
    }

    // Ask the CPU to start loading the slot this key hashes to.
    void prefetch(OrderId key) const { __builtin_prefetch(&slots_[ideal(key)]); }

    // Caller guarantees the key is absent.
    void insert(OrderId key, std::uint32_t val) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        place(key, val);
        ++size_;
    }

    bool erase(OrderId key) {
        std::size_t i = ideal(key);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].val == kEmpty) return false;
            if (slots_[i].key == key) break;
        }
        // Backward shift: pull later entries of the same probe run into the hole.
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].val == kEmpty) break;
            std::size_t k = ideal(slots_[j].key);
            bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (stays) continue;
            slots_[i] = slots_[j];
            i = j;
        }
        slots_[i].val = kEmpty;
        --size_;
        return true;
    }

private:
    struct Slot {
        OrderId key;
        std::uint32_t val;
    };

    std::size_t ideal(OrderId key) const {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void init(std::size_t cap) {
        slots_.assign(cap, Slot{0, kEmpty});
        mask_ = cap - 1;
        shift_ = 64;
        for (std::size_t c = cap; c > 1; c >>= 1) --shift_;
    }

    void place(OrderId key, std::uint32_t val) {
        std::size_t i = ideal(key);
        while (slots_[i].val != kEmpty) i = (i + 1) & mask_;
        slots_[i] = Slot{key, val};
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        init(old.size() * 2);
        for (const Slot& s : old)
            if (s.val != kEmpty) place(s.key, s.val);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    unsigned shift_ = 64;
};

}  // namespace tf
