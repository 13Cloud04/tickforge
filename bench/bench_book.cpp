// Single-threaded matching benchmark: OrderBook vs the std::map ReferenceBook
// on an identical pre-generated op stream.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../tests/reference_book.hpp"
#include "flow.hpp"
#include "tickforge/order_book.hpp"

using namespace tf;
using Clock = std::chrono::steady_clock;

struct Counter {
    std::uint64_t trades = 0, volume = 0;
    void on_trade(const Trade& t) {
        ++trades;
        volume += t.qty;
    }
};

struct Result {
    double ns_per_op;
    std::uint64_t trades, volume;
    std::size_t open, bid_levels, ask_levels;
};

template <class Book>
static Result throughput(const std::vector<Op>& ops) {
    Counter c;
    Book book(c, 1 << 16);
    auto t0 = Clock::now();
    for (const Op& op : ops) apply(book, op);
    auto t1 = Clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return {ns / ops.size(), c.trades, c.volume, book.open_orders(), book.levels(Side::Buy),
            book.levels(Side::Sell)};
}

// Per-operation latency. Timing every op individually would mostly measure
// the clock (Apple Silicon ticks every ~42 ns), so ops are timed in batches
// of 16 and each sample is the batch mean.
template <class Book>
static std::vector<double> latency(const std::vector<Op>& ops) {
    constexpr std::size_t kBatch = 16;
    Counter c;
    Book book(c, 1 << 16);
    std::vector<double> samples;
    samples.reserve(ops.size() / kBatch);
    for (std::size_t i = 0; i + kBatch <= ops.size(); i += kBatch) {
        auto t0 = Clock::now();
        for (std::size_t k = 0; k < kBatch; ++k) apply(book, ops[i + k]);
        auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / kBatch);
    }
    std::sort(samples.begin(), samples.end());
    return samples;
}

static double pct(const std::vector<double>& s, double p) {
    return s[std::min(s.size() - 1, static_cast<std::size_t>(p * s.size()))];
}

int main(int argc, char** argv) {
    FlowConfig cfg;
    cfg.ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
    cfg.target_open = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20'000;
    if (argc > 3) cfg.drift_prob = std::atof(argv[3]);
    if (argc > 4) cfg.level_decay = std::atof(argv[4]);
    const std::vector<Op> ops = make_flow(cfg);
    constexpr int kRuns = 5;

    std::printf("ops=%zu  target_open_orders=%zu  seed=%llu  (best of %d runs)\n\n", cfg.ops,
                cfg.target_open, (unsigned long long)cfg.seed, kRuns);

    Result fast{}, ref{};
    fast.ns_per_op = ref.ns_per_op = 1e18;
    for (int i = 0; i < kRuns; ++i) {
        Result f = throughput<OrderBook<Counter>>(ops);
        Result r = throughput<ReferenceBook<Counter>>(ops);
        if (f.ns_per_op < fast.ns_per_op) fast = f;
        if (r.ns_per_op < ref.ns_per_op) ref = r;
    }
    if (fast.trades != ref.trades || fast.volume != ref.volume) {
        std::fprintf(stderr, "books disagree\n");
        return 1;
    }

    std::printf("%-28s %10s %12s\n", "", "ns/op", "M ops/sec");
    std::printf("%-28s %10.1f %12.2f\n", "tickforge OrderBook", fast.ns_per_op, 1e3 / fast.ns_per_op);
    std::printf("%-28s %10.1f %12.2f\n", "std::map reference", ref.ns_per_op, 1e3 / ref.ns_per_op);
    std::printf("speedup: %.2fx\n\n", ref.ns_per_op / fast.ns_per_op);
    std::printf("trades=%llu  volume=%llu  final: %zu open orders, %zu bid / %zu ask levels\n\n",
                (unsigned long long)fast.trades, (unsigned long long)fast.volume, fast.open,
                fast.bid_levels, fast.ask_levels);

    std::printf("latency per op, ns (mean of 16-op batches)\n");
    std::printf("%-28s %8s %8s %8s %8s\n", "", "p50", "p99", "p99.9", "p99.99");
    auto lf = latency<OrderBook<Counter>>(ops);
    auto lr = latency<ReferenceBook<Counter>>(ops);
    std::printf("%-28s %8.1f %8.1f %8.1f %8.1f\n", "tickforge OrderBook", pct(lf, .5), pct(lf, .99),
                pct(lf, .999), pct(lf, .9999));
    std::printf("%-28s %8.1f %8.1f %8.1f %8.1f\n", "std::map reference", pct(lr, .5), pct(lr, .99),
                pct(lr, .999), pct(lr, .9999));
}
