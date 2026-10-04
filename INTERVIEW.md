# Defending tickforge in an interview

Read the five headers first (about 700 lines), then `bench/itch_replay.cpp`.
Every answer below is something you can point to in the code.

## The 30-second pitch

"I wrote a limit order book in C++20 and measured it against the textbook
std::map version. As a matching engine it does 34 million orders a second,
twice the baseline, and I proved it matches identically by fuzzing both books
side by side for 10 million operations. Then I replayed 33 million messages of
real NASDAQ ITCH data through it: 10.9 million messages a second with zero
inconsistencies, and what the profile of that run showed made me redesign how
price levels are stored."

## Questions you will get

**Why is a sorted vector faster than a map? Insert is O(n).**
Big-O counts steps, not cache misses. In a tree each step is a pointer to a
node somewhere else in memory, likely a cache miss (~100 cycles). In the
vector, the levels near the best price sit next to each other, the CPU
prefetches them, and the scan usually ends in one to three steps because
that is where orders arrive. The O(n) memmove is real but moves 16-byte
slots that are already in cache. I measured it rather than assuming, and on
real data I measured where the assumption breaks (next question).

**When did your first design lose, and what did you do?**
On deep books. I instrumented the level search on real NASDAQ data: 39% of
lookups hit the best price and 84% are within 20 levels, but 5% are more than
64 levels away, in books thousands of levels deep. A pure linear scan pays for
that tail. Two changes: the scan gives up after 16 steps and binary-searches,
and each order now stores the index of its level in a pool, so a cancel or an
execution does not search at all unless it empties the level. The deep-book
benchmark went from 230 to 106 ns per operation.

**Why split a level into a slot and a pool entry?**
The sorted vector has to shift elements on insert and erase, so its elements
should be small and nothing should point into it. The slot is 16 bytes (price
and an index). The level body sits in a pool where its index never changes,
which is what lets orders point at it.

**What is ITCH and how do you build a book from it?**
NASDAQ's order-by-order feed: binary, big-endian, every message prefixed with
its length. It reports what the exchange did: order added, executed, partly
cancelled, deleted, replaced. A book builder applies those as given and never
matches; a crossed book in the feed (an auction, a halted stock) is real and
must be reproduced, not "fixed".

**There is no expected output for real data. How do you know the replay is right?**
Consistency. If my book were wrong, sooner or later the feed would execute or
cancel an order I do not have, or execute more than an order's remaining
size. Over 31.6 million book-changing messages both counts are zero. The feed
operations are also fuzzed against the reference book.

**Explain the prefetching.**
Applying a message is a chain of dependent memory reads: hash slot, then the
order, then its level, each likely a cache miss with 1.7 million live orders
across 8,906 books. The stream is in memory, so I can look ahead: 16 messages
early I prefetch the hash slot; 8 messages early that slot has arrived, so I
read it and prefetch the order. By the time the message is applied its data is
in cache. It took the replay from 135 to 96 ns per message.

**Why did you remove the "maximum throughput" number from the pipeline benchmark?**
Because it measured the wrong thing. With the queue always full the producer
spins reading the consumer's index, the cache line bounces between cores, and
the figure moved between 8 and 23 million depending on where threads landed.
Pacing the sender and sweeping the rate gives a number that means something:
p99 under 2 microseconds up to 15 million messages a second.

**Why indices instead of pointers in the order pool?**
Half the size (4 bytes vs 8), and they stay valid when the vector grows and
moves. Pointers would dangle.

**How does cancel work in O(1)?**
Hash map from order id to pool index, then unlink from a doubly linked list.
Finding the level is the only non-constant part (the short scan).

**Why backward-shift deletion in the hash map?**
With tombstones, a book that adds and cancels forever fills up with dead
slots and lookups get slower until you rehash. Backward shift moves later
entries back into the hole, so the table always looks as if the key was
never inserted.

**Explain the memory orderings in the queue.**
The producer is the only writer of `tail_`, so it reads its own copy
`relaxed`. It publishes with `release` after writing the slot, and the
consumer reads `tail_` with `acquire`; that pair guarantees the consumer sees
the slot contents before it sees the new index. Same in the other direction
for `head_`.

**What is false sharing and where did you avoid it?**
Two threads writing different variables that share a cache line make the line
bounce between cores. `head_` and `tail_` are each aligned to 128 bytes
(Apple Silicon's line size; x86 is 64).

**Why cache the other side's index?**
Without it every push reads `head_`, which the consumer keeps writing, so
every push takes a cache miss. With the cached copy the producer only re-reads
`head_` when the queue looks full.

**How do you know it is correct?**
Differential testing against an implementation simple enough to be obviously
right. Any difference in outcome, trades or book state after any operation
fails the test. Plus ASan, UBSan and TSan.

**Why did you time batches of 16?**
The clock on this machine ticks every ~42 ns and an operation takes ~30 ns.
Timing single operations would report 0 or 42. Say this before they ask; it
shows you know what your numbers mean.

**What is coordinated omission?**
If the load generator falls behind and you timestamp when it actually sent,
you hide the delay the message would have suffered. The pipeline benchmark
stamps each message with its *scheduled* send time.

**What would you add next?**
Sharding instruments across cores for the feed handler, a full trading day
rather than a morning, core pinning and isolation on Linux, and self-trade
prevention in the matching engine.

## Things to try yourself before the interview

1. Change `kLinear` in `order_book.hpp` to 0 (always binary search) and to
   1000 (never), and rerun `make bench` and the ITCH replay.
2. Remove the `alignas(kCacheLine)` in the queue and rerun the pipeline bench.
3. Break the matching loop on purpose (e.g. fill from the tail) and watch the
   differential test catch it.
