// Rebuild every instrument's order book from a real NASDAQ ITCH 5.0 capture.
//
//   gunzip -c data/itch_20191230.part.gz > data/itch.bin   (a cut-off .gz still decompresses up to the cut)
//   ./build/itch_replay data/itch.bin [SYMBOL]
//
// Reports throughput against the std::map reference book on the same bytes,
// and a set of consistency checks: a correct book builder never sees an
// execution, cancel or replace for an order it does not know.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../tests/reference_book.hpp"
#include "tickforge/itch.hpp"
#include "tickforge/order_book.hpp"

using namespace tf;
using Clock = std::chrono::steady_clock;

struct NoTrades {
    void on_trade(const Trade&) {}
};

struct Stats {
    std::uint64_t messages = 0, book_messages = 0;
    std::uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
    std::uint64_t unknown_order = 0, duplicate_add = 0, over_execution = 0;
    std::uint64_t executed_shares = 0;
    std::uint64_t updates_in_hours = 0, crossed_in_hours = 0;
    std::uint64_t first_ts = 0, last_ts = 0;
    std::size_t symbols = 0, peak_open = 0, open_now = 0, max_levels = 0;
};

template <class Book>
struct Replayer {
    NoTrades sink;
    std::vector<std::unique_ptr<Book>> books;  // indexed by stock locate
    std::vector<std::string> names;
    Stats st;
    bool market_open = false;
    bool check_crossed;

    explicit Replayer(bool check) : books(65536), names(65536), check_crossed(check) {}

    Book& book(std::uint16_t locate) {
        auto& b = books[locate];
        if (!b) b = std::make_unique<Book>(sink, 1024);
        return *b;
    }

    // Hints for upcoming messages (see itch::for_each_message_ahead).
    void hint_far(const unsigned char* m) {
        const char t = static_cast<char>(m[0]);
        if (t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U') {
            auto& b = books[itch::be16(m + 1)];
            if (b) b->prefetch_id(itch::be64(m + 11));
        }
    }
    void hint_near(const unsigned char* m) {
        const char t = static_cast<char>(m[0]);
        if (t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U') {
            auto& b = books[itch::be16(m + 1)];
            if (b) b->prefetch_order(itch::be64(m + 11));
        }
    }

    void after_update(Book& b) {
        if (!check_crossed || !market_open) return;
        ++st.updates_in_hours;
        const auto bid = b.best_bid(), ask = b.best_ask();
        if (bid && ask && *bid >= *ask) ++st.crossed_in_hours;
    }

    void on_message(const unsigned char* m, std::size_t len) {
        ++st.messages;
        const itch::Header h = itch::header(m);
        if (len < itch::min_length(h.type)) return;
        if (st.first_ts == 0) st.first_ts = h.timestamp;
        st.last_ts = h.timestamp;
        switch (h.type) {
            case 'A':
            case 'F': {
                Book& b = book(h.locate);
                const Side side = m[19] == 'B' ? Side::Buy : Side::Sell;
                if (b.add_resting(itch::be64(m + 11), side, itch::be32(m + 32), itch::be32(m + 20))) {
                    ++st.adds;
                    st.peak_open = std::max(st.peak_open, ++st.open_now);
                } else {
                    ++st.duplicate_add;
                }
                after_update(b);
                break;
            }
            case 'E':
            case 'C':
            case 'X': {
                Book& b = book(h.locate);
                const Qty shares = itch::be32(m + 19);
                const Exec r = b.execute(itch::be64(m + 11), shares);
                if (h.type == 'X') ++st.cancels;
                else ++st.executions, st.executed_shares += shares;
                if (r == Exec::Unknown) ++st.unknown_order;
                else if (r == Exec::Over) ++st.over_execution;
                if (r == Exec::Full || r == Exec::Over) --st.open_now;
                after_update(b);
                break;
            }
            case 'D': {
                Book& b = book(h.locate);
                ++st.deletes;
                if (b.cancel(itch::be64(m + 11))) --st.open_now;
                else ++st.unknown_order;
                after_update(b);
                break;
            }
            case 'U': {
                Book& b = book(h.locate);
                ++st.replaces;
                if (!b.replace_resting(itch::be64(m + 11), itch::be64(m + 19), itch::be32(m + 31), itch::be32(m + 27)))
                    ++st.unknown_order;
                after_update(b);
                break;
            }
            case 'R':
                names[h.locate].assign(reinterpret_cast<const char*>(m + 11), 8);
                while (!names[h.locate].empty() && names[h.locate].back() == ' ') names[h.locate].pop_back();
                ++st.symbols;
                return;
            case 'S':
                if (m[11] == 'Q') market_open = true;   // start of regular market hours
                if (m[11] == 'M') market_open = false;  // end of regular market hours
                return;
            default:
                return;
        }
        ++st.book_messages;
    }

    void finish() {
        for (auto& b : books)
            if (b) st.max_levels = std::max({st.max_levels, b->levels(Side::Buy), b->levels(Side::Sell)});
    }
};

template <class Book>
static double replay(const unsigned char* data, std::size_t size, bool check, Stats* out,
                     Replayer<Book>** keep = nullptr, bool ahead = false) {
    auto* r = new Replayer<Book>(check);
    const auto t0 = Clock::now();
    if (ahead) itch::for_each_message_ahead(data, size, *r);
    else itch::for_each_message(data, size, *r);
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    r->finish();
    if (out) *out = r->st;
    if (keep) *keep = r;
    else delete r;
    return secs;
}

static std::string clock_time(std::uint64_t ns) {
    char buf[32];
    const std::uint64_t s = ns / 1'000'000'000ull;
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", (unsigned long long)(s / 3600),
                  (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: itch_replay <decompressed ITCH 5.0 file> [SYMBOL]\n");
        return 2;
    }
    const int fd = open(argv[1], O_RDONLY);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb) != 0) {
        std::perror(argv[1]);
        return 1;
    }
    const std::size_t size = static_cast<std::size_t>(sb.st_size);
    const auto* data = static_cast<const unsigned char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (data == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    // Touch every page first so the timed runs measure the book, not the disk.
    std::uint64_t warm = 0;
    for (std::size_t i = 0; i < size; i += 4096) warm += data[i];

    Stats st;
    Replayer<OrderBook<NoTrades>>* fast = nullptr;
    replay<OrderBook<NoTrades>>(data, size, true, &st, &fast);  // one pass with the consistency checks on

#ifdef TICKFORGE_DEPTH_STATS
    std::uint64_t total = 0, cum = 0;
    for (std::uint64_t c : g_depth_hist) total += c;
    std::printf("level lookups: %llu. Distance of the touched level from the best price:\n", (unsigned long long)total);
    for (int d : {0, 1, 2, 4, 9, 19, 63}) {
        cum = 0;
        for (int i = 0; i <= d; ++i) cum += g_depth_hist[i];
        std::printf("  within %2d levels: %6.2f%%\n", d, 100.0 * cum / total);
    }
    (void)warm;
    return 0;
#else
    double best_fast = 1e18, best_ref = 1e18, best_ahead = 1e18;
    Stats ahead_stats;
    for (int i = 0; i < 3; ++i) {
        best_fast = std::min(best_fast, replay<OrderBook<NoTrades>>(data, size, false, nullptr));
        best_ahead = std::min(best_ahead, replay<OrderBook<NoTrades>>(data, size, false, &ahead_stats, nullptr, true));
        best_ref = std::min(best_ref, replay<ReferenceBook<NoTrades>>(data, size, false, nullptr));
    }
    if (ahead_stats.messages != st.messages || ahead_stats.executed_shares != st.executed_shares ||
        ahead_stats.unknown_order != 0 || ahead_stats.peak_open != st.peak_open) {
        std::fprintf(stderr, "prefetching replay disagrees with the plain one\n");
        return 1;
    }

    std::printf("file: %.2f GB, %llu messages, %s to %s\n", size / 1e9, (unsigned long long)st.messages,
                clock_time(st.first_ts).c_str(), clock_time(st.last_ts).c_str());
    std::printf("instruments: %zu   book-changing messages: %llu\n", st.symbols, (unsigned long long)st.book_messages);
    std::printf("  adds %llu  executions %llu  partial cancels %llu  deletes %llu  replaces %llu\n",
                (unsigned long long)st.adds, (unsigned long long)st.executions, (unsigned long long)st.cancels,
                (unsigned long long)st.deletes, (unsigned long long)st.replaces);
    std::printf("  executed volume %llu shares   peak open orders %zu   deepest side %zu levels\n\n",
                (unsigned long long)st.executed_shares, st.peak_open, st.max_levels);

    std::printf("consistency (all should be 0 for a correct book builder):\n");
    std::printf("  events for unknown orders  %llu\n", (unsigned long long)st.unknown_order);
    std::printf("  duplicate order ids        %llu\n", (unsigned long long)st.duplicate_add);
    std::printf("  executions beyond size     %llu\n", (unsigned long long)st.over_execution);
    std::printf("  crossed book after an update during market hours: %llu of %llu (%.4f%%)\n\n",
                (unsigned long long)st.crossed_in_hours, (unsigned long long)st.updates_in_hours,
                st.updates_in_hours ? 100.0 * st.crossed_in_hours / st.updates_in_hours : 0.0);

    std::printf("%-22s %12s %14s\n", "full replay", "seconds", "M msgs/sec");
    std::printf("%-22s %12.2f %14.1f\n", "std::map reference", best_ref, st.messages / best_ref / 1e6);
    std::printf("%-22s %12.2f %14.1f   %.2fx, %.0f ns per book-changing message\n", "tickforge OrderBook", best_fast,
                st.messages / best_fast / 1e6, best_ref / best_fast, best_fast * 1e9 / st.book_messages);
    std::printf("%-22s %12.2f %14.1f   %.2fx, %.0f ns per book-changing message\n", "  + prefetch ahead", best_ahead,
                st.messages / best_ahead / 1e6, best_ref / best_ahead, best_ahead * 1e9 / st.book_messages);

    const std::string want = argc > 2 ? argv[2] : "AAPL";
    for (std::size_t i = 0; i < fast->names.size(); ++i) {
        if (fast->names[i] != want || !fast->books[i]) continue;
        std::vector<LevelView> v;
        std::printf("\n%s at %s (prices in dollars):\n", want.c_str(), clock_time(st.last_ts).c_str());
        fast->books[i]->snapshot(Side::Sell, v, 5);
        for (auto it = v.rbegin(); it != v.rend(); ++it)
            std::printf("            %10.4f  x %-7llu (%u orders)\n", it->price / 1e4, (unsigned long long)it->qty, it->orders);
        std::printf("  ----------------------\n");
        fast->books[i]->snapshot(Side::Buy, v, 5);
        for (const auto& l : v)
            std::printf("  %10.4f            x %-7llu (%u orders)\n", l.price / 1e4, (unsigned long long)l.qty, l.orders);
    }
    (void)warm;
    return 0;
#endif
}
