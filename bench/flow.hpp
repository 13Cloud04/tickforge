#pragma once
// Seeded synthetic order flow. The same stream drives the differential tests
// and the benchmarks, so every number in the README is reproducible.
#include <cstdint>
#include <random>
#include <vector>

#include "tickforge/types.hpp"

namespace tf {

struct Op {
    enum Kind : std::uint8_t { Limit, Market, Cancel, Reduce, Replace };
    Kind kind;
    Side side;
    Tif tif;
    OrderId id;
    OrderId id2;  // Replace: the new id
    Price price;
    Qty qty;
};

struct FlowConfig {
    std::size_t ops = 1'000'000;
    std::uint64_t seed = 42;
    Price start_mid = 100'000;
    std::size_t target_open = 20'000;  // generator steers the book towards this many resting orders
    double drift_prob = 0.10;          // chance per op that the mid moves one tick
    double level_decay = 0.30;         // geometric: how tightly passive orders hug the touch
    bool with_fok = true;
};

// Mix (when the book is at its target size): ~44% passive adds, ~44% cancels,
// 6% marketable limits, 2% market orders, 2% reduces, 2% replaces. Cancels may
// target an order that has since been filled; that is a normal cancel-reject.
inline std::vector<Op> make_flow(const FlowConfig& cfg) {
    std::mt19937_64 rng(cfg.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::geometric_distribution<int> depth(cfg.level_decay);
    std::uniform_int_distribution<Qty> qty(1, 100);

    std::vector<Op> ops;
    ops.reserve(cfg.ops);
    std::vector<OrderId> live;
    live.reserve(cfg.target_open * 2);
    OrderId next_id = 1;
    Price mid = cfg.start_mid;

    auto pick_live = [&](bool remove) {
        std::size_t i = std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng);
        OrderId id = live[i];
        if (remove) {
            live[i] = live.back();
            live.pop_back();
        }
        return id;
    };

    while (ops.size() < cfg.ops) {
        double d = uni(rng);
        if (d < cfg.drift_prob) mid += 1;
        else if (d < 2 * cfg.drift_prob) mid -= 1;

        const Side side = uni(rng) < 0.5 ? Side::Buy : Side::Sell;
        const double cancel_p = live.size() > cfg.target_open ? 0.50 : 0.38;
        double r = uni(rng);

        if (live.empty() || r >= cancel_p + 0.12) {
            const Price off = 1 + depth(rng);
            const Price px = side == Side::Buy ? mid - off : mid + off;
            const OrderId id = next_id++;
            ops.push_back({Op::Limit, side, Tif::GTC, id, 0, px, qty(rng)});
            live.push_back(id);
        } else if (r < cancel_p) {
            ops.push_back({Op::Cancel, side, Tif::GTC, pick_live(true), 0, 0, 0});
        } else if (r < cancel_p + 0.06) {
            // Marketable limit: priced through the touch.
            const Price px = side == Side::Buy ? mid + 2 : mid - 2;
            double t = uni(rng);
            const Tif tif = t < 0.5 ? Tif::GTC : (t < 0.8 || !cfg.with_fok ? Tif::IOC : Tif::FOK);
            const OrderId id = next_id++;
            ops.push_back({Op::Limit, side, tif, id, 0, px, qty(rng) * 3});
            if (tif == Tif::GTC) live.push_back(id);
        } else if (r < cancel_p + 0.08) {
            ops.push_back({Op::Market, side, Tif::IOC, next_id++, 0, 0, qty(rng) * 2});
        } else if (r < cancel_p + 0.10) {
            ops.push_back({Op::Reduce, side, Tif::GTC, pick_live(false), 0, 0, qty(rng) / 4 + 1});
        } else {
            const OrderId old_id = pick_live(true);
            const OrderId id = next_id++;
            const Price off = 1 + depth(rng);
            // Side is a guess; the book uses the original order's side.
            ops.push_back({Op::Replace, side, Tif::GTC, old_id, id, mid + (uni(rng) < 0.5 ? -off : off), qty(rng)});
            live.push_back(id);
        }
    }
    return ops;
}

template <class Book>
inline AddOutcome apply(Book& book, const Op& op) {
    switch (op.kind) {
        case Op::Limit: return book.add_limit(op.id, op.side, op.price, op.qty, op.tif);
        case Op::Market: return book.add_market(op.id, op.side, op.qty);
        case Op::Cancel: return {book.cancel(op.id) ? Status::Cancelled : Status::Rejected, 0};
        case Op::Reduce: return {book.reduce(op.id, op.qty) ? Status::Cancelled : Status::Rejected, 0};
        case Op::Replace: return book.replace(op.id, op.id2, op.price, op.qty);
    }
    return {Status::Rejected, 0};
}

}  // namespace tf
