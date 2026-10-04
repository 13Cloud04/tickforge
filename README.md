# tickforge

A price-time priority limit order book and matching engine in C++20, with a
wait-free single-producer/single-consumer queue feeding it and a decoder for
NASDAQ's ITCH 5.0 market-data feed. Header-only, no dependencies, no
allocation on the hot path after warm-up.

It exists to answer one question with measurements instead of opinions: how
much does data layout matter for an order book, compared with the textbook
`std::map` of `std::list`s? It is measured two ways: as a matching engine on
synthetic order flow, and as a book builder replaying a real exchange feed.

## Results

Apple M5, Apple clang 21, `-O3`.

### Real exchange data: `./build/itch_replay data/itch_20191230.bin`

The first 1.04 GB of NASDAQ's published TotalView-ITCH capture for 30 December
2019 (03:04 to 09:53, so pre-market, the open, and the first 23 minutes of
trading): **33,056,121 messages across 8,906 instruments**, with 1.7 million
orders open at the peak and one book 3,597 price levels deep. Every
instrument's book is rebuilt from the feed.

| Book | Full replay | Messages/sec | Per book-changing message |
|---|---|---|---|
| `std::map` baseline | 14.49 s | 2.3 M | 459 ns |
| tickforge | 4.26 s | 7.8 M | 135 ns (**3.4x**) |
| tickforge + look-ahead prefetch | 3.04 s | **10.9 M** | 96 ns (4.8x) |

Correctness on real data is checked by consistency, since there is no
reference output to compare with. A correct book builder never receives an
event for an order it does not know:

| Check over 31.6 M book-changing messages | Count |
|---|---|
| Executions, cancels or replaces for an unknown order | 0 |
| Duplicate order ids | 0 |
| Executions larger than the order's remaining size | 0 |
| Book crossed after an update during market hours | 139 of 26.2 M (0.0005%; auctions and halted stocks) |

To fetch the data: NASDAQ publishes sample days at
`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`. A partial download works, since
gzip decompresses up to wherever the file was cut:

```
curl -r 0-430698495 -o data/itch.part.gz "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"
gunzip -c < data/itch.part.gz > data/itch_20191230.bin    # stops at the cut with an error; the output is valid
make build/itch_replay && ./build/itch_replay data/itch_20191230.bin AAPL
```

### Matching engine on synthetic flow: `make bench`

10 million operations, seed 42, best of 5 runs.

| Book state | tickforge | `std::map` baseline | Speedup |
|---|---|---|---|
| ~3k open orders, ~160 price levels (default) | **29.1 ns/op, 34.4 M ops/s** | 58.8 ns/op, 17.0 M ops/s | 2.0x |
| ~56k open orders, ~450 levels | 43.0 ns/op, 23.3 M ops/s | 106.1 ns/op, 9.4 M ops/s | 2.5x |
| ~690k open orders, ~1,800 levels | 106 ns/op, 9.4 M ops/s | 308 ns/op, 3.3 M ops/s | 2.9x |

Per-operation latency on the default workload (mean of 16-op batches, because
the Apple Silicon clock ticks every ~42 ns, which is longer than one operation):

| | p50 | p99 | p99.9 | p99.99 |
|---|---|---|---|---|
| tickforge | 26.1 ns | 41.7 ns | 52.1 ns | 391 ns |
| `std::map` baseline | 59.9 ns | 104 ns | 128 ns | 750 ns |

The workload is roughly 44% passive adds, 44% cancels, 6% marketable limits,
2% market orders, 2% reduces, 2% replaces, around a drifting mid price
(`bench/flow.hpp`). About 4.4 M trades are produced per 10 M operations.

### Two-thread pipeline: gateway thread -> SPSC ring -> matching thread

The gateway sends at a fixed rate; latency runs from each message's scheduled
send time to the end of matching.

| Rate | p50 | p99 | p99.9 |
|---|---|---|---|
| 1 M msgs/sec | 250 ns | 375 ns | 5.5 us |
| 5 M | 234 ns | 334 ns | 5.3 us |
| 10 M | 216 ns | 441 ns | 6.3 us |
| 15 M | 301 ns | 1.6 us | 7.5 us |
| 20 M | 1.3 us | 3.5 us | 8.6 us |
| 25 M | 2.0 us | 85 us | 108 us (queue starts to back up) |

## What the real data changed

The first version kept whole price levels in the sorted vector and searched
for the level on every cancel. It matched the synthetic benchmark's
assumptions well. Then I replayed real data and profiled it:

1. **The long tail is real.** I instrumented the level search
   (`make build/itch_depth`). On real flow 39% of lookups hit the best price
   and 84% land within 20 levels, but 5% are more than 64 levels from the
   touch, in books thousands of levels deep. The search is now a 16-step
   linear scan from the best price that falls back to binary search.
2. **Most events should not search at all.** A third of the replay time was
   in the level search, mostly for cancels and executions. Levels now live in
   a pool and never move; each order stores its level's index; the sorted
   vector holds only 16-byte (price, level) slots. A cancel goes straight to
   its level and touches the sorted list only if the level empties. Level
   searches fell from 34.6 M to 24.2 M on this capture.
3. **The rest is memory latency.** With 8,906 books and 1.7 M live orders,
   each message is a chain of cache misses: hash slot, then order, then level.
   The replay keeps two cursors ahead of the message being applied and issues
   prefetches at 16 and 8 messages' distance, so the loads are already under
   way when a message's turn comes.

| Change | ITCH replay | Deep synthetic book (690k orders) |
|---|---|---|
| First version | 4.82 s | 230 ns/op (1.6x baseline) |
| + binary-search fallback, orders point at stable levels | 4.26 s | 106 ns/op (2.9x) |
| + look-ahead prefetch | 3.04 s | n/a |

The deep-book case was the stated weakness of the first design. It is now the
case with the largest lead over the baseline.

One more lesson, from the benchmark rather than the book: the pipeline
benchmark originally reported an "as fast as possible" number that swung
between 8 and 23 M msgs/sec. With the queue permanently full, the producer
spins on the consumer's index and the two cores fight over that cache line;
it measured the contention, not the engine. It also read the clock for every
message, which costs about as much as matching one. The benchmark now paces
the sender, times one message in eight, and sweeps the rate.

## Correctness

Speed numbers for a matching engine mean nothing if it matches wrongly, so the
fast book is checked against the slow, obviously-correct one:

- **Differential fuzzing** (`tests/test_differential.cpp`): both books consume
  the same seeded stream. After *every* operation the outcome, the emitted
  trades and the top of book must be identical, and full-depth snapshots are
  compared every 4,096 operations. Default run: 40 seeds, 10 M operations,
  3.5 M trades, 0 divergences.
- **Feed fuzzing** (`tests/test_feed.cpp`): the same for the book-building
  operations (rest, execute, cancel, replace without matching), 4 M operations
  over a price range wide enough to exercise the binary-search path, plus
  decoder tests on hand-encoded ITCH messages including a truncated capture.
- **Scenario tests** (`tests/test_book.cpp`): price-time priority, partial
  fills, IOC, FOK, market sweeps, cancel/reduce/replace queue-position rules,
  duplicate-id rejection.
- **Sanitizers**: `make asan` runs the tests under AddressSanitizer +
  UBSan; `make tsan` runs the queue test under ThreadSanitizer. Both clean.
- **Queue test** (`tests/test_spsc.cpp`): 20 M sequenced items across two
  threads through a 1,024-slot ring, checked for loss, duplication and order.

```
make test    # all of the above except sanitizers
make asan
make tsan
```

## Design

**Orders** live in one contiguous pool (`std::vector<Node>`, 40 bytes each)
and are linked into per-price FIFO queues with 32-bit indices rather than
pointers. Freed slots go on a free list, so steady state never calls `malloc`.

**Price levels** are split in two. The sorted part is a `std::vector` of
16-byte `(price, level index)` slots per side with the best price at the
*back*: eight prices per cache line while scanning, and removing the best
price is `pop_back`. The level itself (FIFO head and tail, total quantity)
sits in a pool at a fixed index that every order in it records.

**Order lookup** is a linear-probing hash map with backward-shift deletion
(`id_map.hpp`). No tombstones, so add/cancel churn never degrades it.

**Trade reporting** goes through a template parameter (`Handler::on_trade`),
which the compiler inlines. There is no virtual dispatch on the hot path.

**The queue** (`spsc_queue.hpp`) gives each thread its own atomic index on its
own 128-byte-aligned cache line, plus a private cached copy of the other
side's index. A push or pop normally touches no memory the other thread
writes; the shared index is only re-read when the queue looks full or empty.

**The feed decoder** (`itch.hpp`) reads length-prefixed big-endian messages in
place from a memory-mapped file: no copies, no allocation.

Matching supports limit (GTC / IOC / FOK), market, cancel, reduce (keeps queue
position) and replace (loses it); trades print at the resting order's price.
Book building supports the feed's add, execute, partial cancel, delete and
replace, applied as reported without matching.

## Limits

- The ITCH capture covers 03:04 to 09:53 of one day (the first 430 MB of a
  3.5 GB file), not a full session, and the replay is single-threaded over
  all instruments. A production feed handler would shard instruments across
  cores.
- The matching engine is single instrument, single thread. No persistence, no
  risk checks, no network gateway, no self-trade prevention.
- The prefetch distances (16 and 8 messages) were tuned on this machine.
- Latency was measured on a laptop without core pinning or isolation, which
  macOS does not offer. Tail numbers on a tuned Linux box would differ.

## Layout

```
include/tickforge/   order_book.hpp  id_map.hpp  spsc_queue.hpp  itch.hpp  types.hpp
tests/               scenario tests, differential fuzzers, std::map reference book, queue test
bench/               flow generator, matching, pipeline and ITCH replay benchmarks
examples/demo.cpp    type orders, see trades and the book
```

```
make && printf 'sell 1 101 5\nbuy 2 99 10\nbuy 3 101 7\nbook\n' | ./build/demo
```
