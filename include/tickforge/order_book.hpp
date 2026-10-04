#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "id_map.hpp"
#include "types.hpp"

// Optional instrumentation: define TICKFORGE_DEPTH_STATS to record how many
// levels from the best price each level lookup had to scan. Compiled out by
// default, so the benchmarks measure the uninstrumented book.
#ifdef TICKFORGE_DEPTH_STATS
namespace tf {
inline std::uint64_t g_depth_hist[65] = {};
}
#define TICKFORGE_RECORD_DEPTH(d) (++::tf::g_depth_hist[(d) < 64 ? (d) : 64])
#else
#define TICKFORGE_RECORD_DEPTH(d) ((void)0)
#endif

namespace tf {

// Price-time priority limit order book for a single instrument.
//
// Layout, chosen for cache behaviour rather than asymptotics:
//   * Orders live in one contiguous pool and are linked into per-price FIFO
//     queues by 32-bit indices. No allocation after warm-up.
//   * Each side keeps its prices in a sorted std::vector of 16-byte slots with
//     the best price at the BACK. Most activity happens within a few ticks of
//     the touch, so finding or inserting a price is a short scan over adjacent
//     memory, and removing the best one is a pop_back. Deep lookups fall back
//     to binary search.
//   * The levels themselves (FIFO head/tail, totals) live in a pool and never
//     move. Every order stores its level's index, so a cancel or an execution
//     goes straight to its level and touches the sorted list only if the
//     level becomes empty.
//   * OrderId -> pool index goes through a linear-probing hash map.
//
// Handler is any type with `void on_trade(const Trade&)`. It is a template
// parameter so the call is inlined; there are no virtual calls on the hot path.
// Not thread-safe: one book is owned by one thread (see spsc_queue.hpp).
template <class Handler>
class OrderBook {
public:
    explicit OrderBook(Handler& handler, std::size_t expected_orders = 1 << 16)
        : handler_(handler), ids_(expected_orders) {
        pool_.reserve(expected_orders);
        bids_.reserve(256);
        asks_.reserve(256);
        levels_.reserve(512);
    }

    AddOutcome add_limit(OrderId id, Side side, Price price, Qty qty, Tif tif = Tif::GTC) {
        if (qty == 0 || ids_.find(id) != IdMap::kEmpty) return {Status::Rejected, 0};
        if (tif == Tif::FOK && !can_fill(side, price, qty)) return {Status::Rejected, 0};

        const Qty left = match(id, side, price, qty);
        const Qty filled = qty - left;
        if (left == 0) return {Status::Filled, filled};
        if (tif != Tif::GTC) return {Status::Cancelled, filled};
        rest(id, side, price, left);
        return {Status::Rested, filled};
    }

    AddOutcome add_market(OrderId id, Side side, Qty qty) {
        const Price p = side == Side::Buy ? std::numeric_limits<Price>::max()
                                          : std::numeric_limits<Price>::min();
        return add_limit(id, side, p, qty, Tif::IOC);
    }

    bool cancel(OrderId id) {
        const std::uint32_t idx = ids_.find(id);
        if (idx == IdMap::kEmpty) return false;
        unlink(idx);
        ids_.erase(id);
        release(idx);
        return true;
    }

    // Shrink a resting order in place, keeping its queue position.
    // Reducing by the full remaining quantity (or more) cancels it.
    bool reduce(OrderId id, Qty by) {
        const std::uint32_t idx = ids_.find(id);
        if (idx == IdMap::kEmpty || by == 0) return false;
        Node& n = pool_[idx];
        if (by >= n.qty) return cancel(id);
        levels_[n.level].total -= by;
        n.qty -= by;
        return true;
    }

    // Cancel/replace: the new order goes to the back of its queue.
    AddOutcome replace(OrderId id, OrderId new_id, Price price, Qty qty) {
        const std::uint32_t idx = ids_.find(id);
        if (idx == IdMap::kEmpty || qty == 0) return {Status::Rejected, 0};
        if (new_id != id && ids_.find(new_id) != IdMap::kEmpty) return {Status::Rejected, 0};
        const Side side = pool_[idx].side;
        cancel(id);
        return add_limit(new_id, side, price, qty);
    }

    // ---- Book building from an exchange feed --------------------------
    // A market-data feed reports what the exchange already did, so these
    // apply events as given and never match.

    // Rest an order exactly as reported. False on zero quantity or duplicate id.
    bool add_resting(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0 || ids_.find(id) != IdMap::kEmpty) return false;
        rest(id, side, price, qty);
        return true;
    }

    // Take `qty` off a resting order (an execution or a partial cancel).
    Exec execute(OrderId id, Qty qty) {
        const std::uint32_t idx = ids_.find(id);
        if (idx == IdMap::kEmpty) return Exec::Unknown;
        Node& n = pool_[idx];
        if (qty < n.qty) {
            levels_[n.level].total -= qty;
            n.qty -= qty;
            return Exec::Partial;
        }
        const Exec r = qty == n.qty ? Exec::Full : Exec::Over;
        cancel(id);
        return r;
    }

    // Replace as a feed reports it: same side, new id, price and size, no matching.
    bool replace_resting(OrderId id, OrderId new_id, Price price, Qty qty) {
        const std::uint32_t idx = ids_.find(id);
        if (idx == IdMap::kEmpty) return false;
        const Side side = pool_[idx].side;
        cancel(id);
        return add_resting(new_id, side, price, qty);
    }

    // ---- Prefetch hints ------------------------------------------------
    // A feed handler can see the next few messages before it applies them.
    // An order lookup is two dependent memory reads (hash slot, then the
    // order), each likely a cache miss when hundreds of thousands of orders
    // are live, so the two hints are issued at different distances ahead.

    void prefetch_id(OrderId id) const { ids_.prefetch(id); }

    void prefetch_order(OrderId id) const {
        const std::uint32_t idx = ids_.find(id);
        if (idx != IdMap::kEmpty) __builtin_prefetch(&pool_[idx]);
    }

    std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.back().price);
    }
    std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.back().price);
    }
    std::size_t open_orders() const { return ids_.size(); }
    std::size_t levels(Side s) const { return side(s).size(); }

    // Best-first view of one side, at most `max_levels` deep.
    void snapshot(Side s, std::vector<LevelView>& out,
                  std::size_t max_levels = std::numeric_limits<std::size_t>::max()) const {
        out.clear();
        const auto& slots = side(s);
        for (std::size_t i = slots.size(); i-- > 0 && out.size() < max_levels;) {
            const Level& lvl = levels_[slots[i].level];
            out.push_back({slots[i].price, lvl.total, lvl.count});
        }
    }

private:
    static constexpr std::uint32_t kNil = 0xFFFFFFFFu;
    static constexpr std::size_t kLinear = 16;  // levels scanned linearly before falling back to binary search

    struct Node {
        OrderId id;
        Price price;
        Qty qty;
        std::uint32_t next;
        std::uint32_t prev;
        std::uint32_t level;  // index into levels_: an order reaches its level without searching
        Side side;
    };

    // One price level: a FIFO of orders. Levels live in a pool and never move.
    struct Level {
        Qty total;
        std::uint32_t head;
        std::uint32_t tail;
        std::uint32_t count;
        std::uint32_t next_free;
    };

    // What is kept in sorted order: 16 bytes, so eight prices share a cache line
    // while scanning, and inserting or removing a price moves very little.
    struct Slot {
        Price price;
        std::uint32_t level;
    };

    using Slots = std::vector<Slot>;

    Slots& side(Side s) { return s == Side::Buy ? bids_ : asks_; }
    const Slots& side(Side s) const { return s == Side::Buy ? bids_ : asks_; }

    // True when a resting level at `level_price` is strictly more aggressive
    // than `p` for that side (higher bid / lower ask). Prices are stored from
    // least to most aggressive, so the best price is always at the back.
    static bool more_aggressive(Side s, Price level_price, Price p) {
        return s == Side::Buy ? level_price > p : level_price < p;
    }

    static bool crosses(Side taker, Price taker_price, Price maker_price) {
        return taker == Side::Buy ? taker_price >= maker_price : taker_price <= maker_price;
    }

    bool can_fill(Side taker, Price price, Qty qty) const {
        const auto& opp = side(opposite(taker));
        Qty avail = 0;
        for (std::size_t i = opp.size(); i-- > 0;) {
            if (!crosses(taker, price, opp[i].price)) break;
            avail += levels_[opp[i].level].total;
            if (avail >= qty) return true;
        }
        return false;
    }

    // Match an incoming order against the opposite side; returns unfilled qty.
    Qty match(OrderId taker, Side taker_side, Price price, Qty qty) {
        auto& opp = side(opposite(taker_side));
        while (qty != 0 && !opp.empty()) {
            const Slot best = opp.back();
            if (!crosses(taker_side, price, best.price)) break;
            Level& lvl = levels_[best.level];
            while (qty != 0 && lvl.head != kNil) {
                const std::uint32_t idx = lvl.head;
                Node& maker = pool_[idx];
                const Qty fill = qty < maker.qty ? qty : maker.qty;
                maker.qty -= fill;
                lvl.total -= fill;
                qty -= fill;
                handler_.on_trade(Trade{maker.id, taker, best.price, fill});
                if (maker.qty == 0) {
                    lvl.head = maker.next;
                    if (lvl.head != kNil) pool_[lvl.head].prev = kNil;
                    else lvl.tail = kNil;
                    --lvl.count;
                    ids_.erase(maker.id);
                    release(idx);
                }
            }
            if (lvl.head == kNil) {
                release_level(best.level);
                opp.pop_back();
            }
        }
        return qty;
    }

    void rest(OrderId id, Side s, Price price, Qty qty) {
        const std::uint32_t idx = acquire();
        ids_.insert(id, idx);

        auto& slots = side(s);
        const std::size_t i = locate(slots, s, price);
        if (i > 0 && slots[i - 1].price == price) {
            const std::uint32_t li = slots[i - 1].level;
            Level& lvl = levels_[li];
            pool_[idx] = Node{id, price, qty, kNil, lvl.tail, li, s};
            pool_[lvl.tail].next = idx;
            lvl.tail = idx;
            lvl.total += qty;
            ++lvl.count;
        } else {
            const std::uint32_t li = acquire_level();
            levels_[li] = Level{qty, idx, idx, 1, kNil};
            pool_[idx] = Node{id, price, qty, kNil, kNil, li, s};
            slots.insert(slots.begin() + static_cast<std::ptrdiff_t>(i), Slot{price, li});
        }
    }

    // Number of prices that are NOT more aggressive than `price`; equivalently
    // the index where a new price belongs. If exactly `price` is present it is
    // at index (result - 1).
    //
    // Most lookups land within a few levels of the best price, so start with a
    // short linear scan from the back, which is all in cache. Real exchange
    // data also has a long tail (books thousands of levels deep, orders placed
    // far from the market), so past kLinear steps switch to binary search.
    static std::size_t locate(const Slots& slots, Side s, Price price) {
        std::size_t i = slots.size();
        const std::size_t stop = i > kLinear ? i - kLinear : 0;
        while (i > stop && more_aggressive(s, slots[i - 1].price, price)) --i;
        if (i == stop && i > 0 && more_aggressive(s, slots[i - 1].price, price)) {
            std::size_t lo = 0, hi = i;  // first index in [0, i) whose price is more aggressive
            while (lo < hi) {
                const std::size_t mid = lo + (hi - lo) / 2;
                if (more_aggressive(s, slots[mid].price, price)) hi = mid;
                else lo = mid + 1;
            }
            i = lo;
        }
        TICKFORGE_RECORD_DEPTH(slots.size() - i);
        return i;
    }

    // Take an order out of its level. Only if that empties the level does the
    // sorted price list need to be searched and changed.
    void unlink(std::uint32_t idx) {
        const Node& n = pool_[idx];
        Level& lvl = levels_[n.level];
        if (n.prev != kNil) pool_[n.prev].next = n.next;
        else lvl.head = n.next;
        if (n.next != kNil) pool_[n.next].prev = n.prev;
        else lvl.tail = n.prev;
        lvl.total -= n.qty;
        if (--lvl.count == 0) {
            auto& slots = side(n.side);
            slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(locate(slots, n.side, n.price) - 1));
            release_level(n.level);
        }
    }

    std::uint32_t acquire_level() {
        if (free_level_ != kNil) {
            const std::uint32_t li = free_level_;
            free_level_ = levels_[li].next_free;
            return li;
        }
        levels_.emplace_back();
        return static_cast<std::uint32_t>(levels_.size() - 1);
    }

    void release_level(std::uint32_t li) {
        levels_[li].next_free = free_level_;
        free_level_ = li;
    }

    std::uint32_t acquire() {
        if (free_ != kNil) {
            const std::uint32_t idx = free_;
            free_ = pool_[idx].next;
            return idx;
        }
        pool_.emplace_back();
        return static_cast<std::uint32_t>(pool_.size() - 1);
    }

    void release(std::uint32_t idx) {
        pool_[idx].next = free_;
        free_ = idx;
    }

    Handler& handler_;
    std::vector<Node> pool_;
    std::uint32_t free_ = kNil;
    IdMap ids_;
    std::vector<Level> levels_;
    std::uint32_t free_level_ = kNil;
    Slots bids_;  // ascending price, best (highest) at the back
    Slots asks_;  // descending price, best (lowest) at the back
};

}  // namespace tf
