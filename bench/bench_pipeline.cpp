// Two-thread pipeline: a gateway thread publishes orders into the SPSC ring,
// the matching thread consumes and matches them.
// The gateway sends at a fixed rate and each message is timed from its
// scheduled send time to the moment matching finishes. Raising the rate until
// the latency stops being flat finds what the pipeline can sustain.
//
// (An earlier version also reported an "as fast as possible" number. With the
// queue permanently full the producer spins on the consumer's index, the two
// cores fight over that cache line, and the result swung between 8 and 23 M/s
// depending on thread placement. It measured the contention, not the engine.)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "flow.hpp"
#include "tickforge/order_book.hpp"
#include "tickforge/spsc_queue.hpp"

using namespace tf;
using Clock = std::chrono::steady_clock;

struct Counter {
    std::uint64_t trades = 0;
    void on_trade(const Trade&) { ++trades; }
};

struct Msg {
    Op op;
    Clock::time_point sent;
};

static double pct(const std::vector<double>& s, double p) {
    return s[std::min(s.size() - 1, static_cast<std::size_t>(p * s.size()))];
}

static void run(const std::vector<Op>& ops, double rate_per_sec) {
    SpscQueue<Msg> q(1 << 16);
    std::vector<double> lat;
    lat.reserve(ops.size());
    std::atomic<bool> ready{false};
    std::uint64_t trades = 0;

    std::thread engine([&] {
        // The book lives on the matching thread's own stack, away from the
        // producer's loop variables.
        Counter c;
        OrderBook<Counter> book(c, 1 << 16);
        Msg m;
        ready.store(true);
        for (std::size_t done = 0; done < ops.size();) {
            if (!q.try_pop(m)) continue;  // busy-spin: a matching thread never sleeps
            apply(book, m.op);
            // Reading the clock costs about as much as matching an order, so
            // time one message in eight rather than slow the thread being measured.
            if ((done & 7) == 0)
                lat.push_back(std::chrono::duration<double, std::nano>(Clock::now() - m.sent).count());
            ++done;
        }
        trades = c.trades;
    });
    while (!ready.load()) {}

    const auto t0 = Clock::now();
    const std::chrono::duration<double, std::nano> gap(1e9 / rate_per_sec);
    auto now = t0;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        Msg m{ops[i], {}};
        const auto due = t0 + std::chrono::duration_cast<Clock::duration>(gap * i);
        while (now < due) now = Clock::now();  // when behind schedule this reads no clock at all
        // Stamp with the scheduled time, not the actual send time, so a
        // stalled producer cannot hide queueing delay (coordinated omission).
        m.sent = due;
        while (!q.try_push(m)) {}
    }
    engine.join();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    std::sort(lat.begin(), lat.end());
    const double achieved = ops.size() / secs / 1e6;
    const bool kept_up = achieved > rate_per_sec / 1e6 * 0.98 && pct(lat, .99) < 1e6;
    std::printf("%6.1f M/s  %10.0f %10.0f %10.0f %12.0f   %s\n", rate_per_sec / 1e6, pct(lat, .5), pct(lat, .99),
                pct(lat, .999), lat.back(), kept_up ? "" : "saturated: queue backs up");
    (void)trades;
}

int main(int argc, char** argv) {
    FlowConfig cfg;
    cfg.ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
    const std::vector<Op> ops = make_flow(cfg);
    std::printf("ops=%zu, queue capacity 65536, sizeof(Msg)=%zu bytes\n", ops.size(), sizeof(Msg));
    std::printf("%10s  %10s %10s %10s %12s\n", "rate", "p50 ns", "p99 ns", "p99.9 ns", "max ns");
    for (double rate : {1e6, 5e6, 10e6, 15e6, 20e6, 25e6}) run(ops, rate);
}
