// Differential fuzzing: drive OrderBook and the std::map-based ReferenceBook
// with the same random stream and require identical behaviour after every
// single operation (outcome, trades, top of book) plus full-depth snapshots
// at intervals.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../bench/flow.hpp"
#include "check.hpp"
#include "reference_book.hpp"
#include "tickforge/order_book.hpp"

using namespace tf;

struct Recorder {
    std::vector<Trade> trades;
    void on_trade(const Trade& t) { trades.push_back(t); }
};

static bool same(const Trade& a, const Trade& b) {
    return a.maker == b.maker && a.taker == b.taker && a.price == b.price && a.qty == b.qty;
}

static std::size_t run(std::uint64_t seed, std::size_t n, std::size_t target_open, double decay) {
    FlowConfig cfg;
    cfg.ops = n;
    cfg.seed = seed;
    cfg.target_open = target_open;
    cfg.level_decay = decay;
    const std::vector<Op> ops = make_flow(cfg);

    Recorder ra, rb;
    OrderBook<Recorder> fast(ra, 64);  // tiny initial capacity: exercises pool and map growth
    ReferenceBook<Recorder> ref(rb);
    std::vector<LevelView> sa, sb;
    std::size_t trades = 0;

    for (std::size_t i = 0; i < ops.size(); ++i) {
        ra.trades.clear();
        rb.trades.clear();
        AddOutcome oa = apply(fast, ops[i]);
        AddOutcome ob = apply(ref, ops[i]);
        CHECK(oa.status == ob.status && oa.filled == ob.filled);
        CHECK(ra.trades.size() == rb.trades.size());
        for (std::size_t k = 0; k < ra.trades.size(); ++k) CHECK(same(ra.trades[k], rb.trades[k]));
        trades += ra.trades.size();
        CHECK(fast.best_bid() == ref.best_bid());
        CHECK(fast.best_ask() == ref.best_ask());
        CHECK(fast.open_orders() == ref.open_orders());
        // A resting book must never be crossed.
        if (fast.best_bid() && fast.best_ask()) CHECK(*fast.best_bid() < *fast.best_ask());
        if (i % 4096 == 0 || i + 1 == ops.size()) {
            for (Side s : {Side::Buy, Side::Sell}) {
                fast.snapshot(s, sa);
                ref.snapshot(s, sb);
                CHECK(sa == sb);
            }
        }
    }
    return trades;
}

int main(int argc, char** argv) {
    const std::size_t seeds = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 40;
    const std::size_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 250'000;
    std::size_t total_ops = 0, total_trades = 0;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        // Alternate between a deep, sparse book and a tiny, constantly crossing one.
        const bool tight = seed % 2 == 0;
        total_trades += run(seed, n, tight ? 50 : 5'000, tight ? 0.6 : 0.2);
        total_ops += n;
    }
    std::printf("test_differential: %zu seeds, %zu ops, %zu trades, 0 divergences\n", seeds,
                total_ops, total_trades);
}
