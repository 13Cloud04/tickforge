// Two real threads push a strictly increasing sequence through the queue;
// the consumer checks that nothing is lost, duplicated or reordered.
// Build with `make tsan` to run it under ThreadSanitizer.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "check.hpp"
#include "tickforge/spsc_queue.hpp"

using namespace tf;

int main(int argc, char** argv) {
    const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20'000'000;

    SpscQueue<std::uint64_t> small(3);
    CHECK(small.capacity() == 4);
    std::uint64_t v = 0;
    CHECK(!small.try_pop(v));
    for (std::uint64_t i = 0; i < 4; ++i) CHECK(small.try_push(i));
    CHECK(!small.try_push(99));  // full
    for (std::uint64_t i = 0; i < 4; ++i) CHECK(small.try_pop(v) && v == i);
    CHECK(!small.try_pop(v));

    SpscQueue<std::uint64_t> q(1024);  // small on purpose: forces constant wrap-around and full/empty races
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < n; ++i)
            while (!q.try_push(i)) {}
    });
    std::uint64_t expect = 0, got = 0;
    while (expect < n) {
        if (q.try_pop(got)) {
            CHECK(got == expect);
            ++expect;
        }
    }
    producer.join();
    CHECK(!q.try_pop(got));
    std::printf("test_spsc: %llu items in order across 2 threads\n", (unsigned long long)n);
}
