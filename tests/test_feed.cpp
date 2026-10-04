// Book building from an exchange feed: the feed operations on the book, the
// ITCH 5.0 decoder, and a differential fuzz of feed operations against the
// reference book.
#include <cstring>
#include <random>
#include <vector>

#include "check.hpp"
#include "reference_book.hpp"
#include "tickforge/itch.hpp"
#include "tickforge/order_book.hpp"

using namespace tf;

struct Sink {
    std::size_t trades = 0;
    void on_trade(const Trade&) { ++trades; }
};

static void feed_operations() {
    Sink s;
    OrderBook<Sink> b(s);
    CHECK(b.add_resting(1, Side::Buy, 100, 50));
    CHECK(b.add_resting(2, Side::Sell, 99, 30));   // a feed may show a crossed book (auctions, halts)...
    CHECK(s.trades == 0);                           // ...and the builder must not match it
    CHECK(!b.add_resting(1, Side::Buy, 101, 5) && !b.add_resting(3, Side::Buy, 101, 0));
    CHECK(b.best_bid() == 100 && b.best_ask() == 99);

    CHECK(b.execute(1, 20) == Exec::Partial);
    std::vector<LevelView> v;
    b.snapshot(Side::Buy, v);
    CHECK(v.size() == 1 && v[0] == (LevelView{100, 30, 1}));
    CHECK(b.execute(1, 30) == Exec::Full && !b.best_bid());
    CHECK(b.execute(1, 1) == Exec::Unknown);
    CHECK(b.execute(2, 31) == Exec::Over && b.open_orders() == 0);

    CHECK(b.add_resting(4, Side::Sell, 105, 10) && b.add_resting(5, Side::Sell, 105, 10));
    CHECK(b.replace_resting(4, 6, 104, 7));         // keeps the side, takes the new id
    CHECK(!b.replace_resting(4, 7, 104, 7));        // the old id is gone
    b.snapshot(Side::Sell, v);
    CHECK(v.size() == 2 && v[0] == (LevelView{104, 7, 1}) && v[1] == (LevelView{105, 10, 1}));
    CHECK(b.cancel(6) && b.cancel(5) && b.levels(Side::Sell) == 0);
}

// ---- ITCH encoding helpers (big-endian) ----
static void put(std::vector<unsigned char>& out, std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

static void message(std::vector<unsigned char>& file, char type, std::uint16_t locate, std::uint64_t ts,
                    const std::vector<unsigned char>& body) {
    put(file, 11 + body.size(), 2);
    file.push_back(static_cast<unsigned char>(type));
    put(file, locate, 2);
    put(file, 0, 2);
    put(file, ts, 6);
    file.insert(file.end(), body.begin(), body.end());
}

static std::vector<unsigned char> add_body(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price) {
    std::vector<unsigned char> b;
    put(b, ref, 8);
    b.push_back(static_cast<unsigned char>(side));
    put(b, shares, 4);
    for (char c : std::string("TEST    ")) b.push_back(static_cast<unsigned char>(c));
    put(b, price, 4);
    return b;
}

struct Decoded {
    std::vector<char> types;
    std::vector<std::uint64_t> refs;
    std::uint64_t last_ts = 0;
    std::uint32_t price = 0, shares = 0;
    std::uint16_t locate = 0;
    std::size_t far = 0, near = 0;
    void on_message(const unsigned char* m, std::size_t len) {
        const itch::Header h = itch::header(m);
        CHECK(len >= itch::min_length(h.type));
        types.push_back(h.type);
        last_ts = h.timestamp;
        locate = h.locate;
        if (h.type == 'A') {
            refs.push_back(itch::be64(m + 11));
            shares = itch::be32(m + 20);
            price = itch::be32(m + 32);
            CHECK(m[19] == 'S' && std::memcmp(m + 24, "TEST    ", 8) == 0);
        }
    }
    void hint_far(const unsigned char*) { ++far; }
    void hint_near(const unsigned char*) { ++near; }
};

static void itch_decoder() {
    std::vector<unsigned char> file;
    message(file, 'S', 0, 1, {'Q'});
    for (std::uint64_t i = 0; i < 40; ++i)
        message(file, 'A', 7, 34'200'000'000'123ull + i, add_body(0x0102030405060708ull + i, 'S', 300, 1'234'500));
    std::vector<unsigned char> del;
    put(del, 0x0102030405060708ull, 8);
    message(file, 'D', 7, 34'200'000'000'999ull, del);
    const std::size_t complete = file.size();
    put(file, 36, 2);  // a final message cut off mid-way, as in a truncated capture
    file.push_back('A');

    for (bool ahead : {false, true}) {
        Decoded d;
        const std::size_t used = ahead ? itch::for_each_message_ahead(file.data(), file.size(), d)
                                       : itch::for_each_message(file.data(), file.size(), d);
        CHECK(used == complete);
        CHECK(d.types.size() == 42 && d.types.front() == 'S' && d.types.back() == 'D');
        CHECK(d.refs.size() == 40 && d.refs[0] == 0x0102030405060708ull && d.refs[39] == 0x0102030405060708ull + 39);
        CHECK(d.locate == 7 && d.shares == 300 && d.price == 1'234'500 && d.last_ts == 34'200'000'000'999ull);
        if (ahead) CHECK(d.far == 42 && d.near == 42);  // every message was hinted exactly once
    }
}

template <class Book>
struct Driver {
    Sink sink;
    Book book{sink, 64};
};

static void differential_feed(std::uint64_t seed, std::size_t n) {
    std::mt19937_64 rng(seed);
    Driver<OrderBook<Sink>> fast;
    Driver<ReferenceBook<Sink>> ref;
    std::vector<OrderId> live;
    OrderId next = 1;
    std::vector<LevelView> a, b;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned r = rng() % 100;
        if (live.empty() || r < 45) {
            const Side side = rng() % 2 ? Side::Buy : Side::Sell;
            // A wide price range: exercises both the linear scan and the binary-search fallback.
            const Price px = 10'000 + static_cast<Price>(rng() % (rng() % 4 ? 12 : 600));
            const Qty q = 1 + rng() % 500;
            const OrderId id = rng() % 50 == 0 && !live.empty() ? live[rng() % live.size()] : next++;
            const bool x = fast.book.add_resting(id, side, px, q), y = ref.book.add_resting(id, side, px, q);
            CHECK(x == y);
            if (x) live.push_back(id);
        } else {
            const std::size_t k = rng() % live.size();
            const OrderId id = rng() % 40 == 0 ? next + 1000 : live[k];
            if (r < 70) {
                const Qty q = 1 + rng() % 400;
                const Exec x = fast.book.execute(id, q), y = ref.book.execute(id, q);
                CHECK(x == y);
                if (x == Exec::Full || x == Exec::Over) live[k] = live.back(), live.pop_back();
            } else if (r < 90) {
                const bool x = fast.book.cancel(id), y = ref.book.cancel(id);
                CHECK(x == y);
                if (x) live[k] = live.back(), live.pop_back();
            } else {
                const OrderId nid = next++;
                const Price px = 10'000 + static_cast<Price>(rng() % 600);
                const Qty q = 1 + rng() % 500;
                const bool x = fast.book.replace_resting(id, nid, px, q), y = ref.book.replace_resting(id, nid, px, q);
                CHECK(x == y);
                if (x) live[k] = nid;
            }
        }
        CHECK(fast.book.open_orders() == ref.book.open_orders());
        CHECK(fast.book.best_bid() == ref.book.best_bid() && fast.book.best_ask() == ref.book.best_ask());
        if (i % 2048 == 0 || i + 1 == n) {
            for (Side s : {Side::Buy, Side::Sell}) {
                fast.book.snapshot(s, a);
                ref.book.snapshot(s, b);
                CHECK(a == b);
            }
        }
    }
    CHECK(fast.sink.trades == 0 && ref.sink.trades == 0);
}

int main() {
    feed_operations();
    itch_decoder();
    for (std::uint64_t seed = 1; seed <= 20; ++seed) differential_feed(seed, 200'000);
    std::puts("test_feed: feed operations, ITCH decoder, 4000000 fuzzed feed operations with 0 divergences");
}
