// Hand-written scenarios for the matching rules.
#include <vector>

#include "check.hpp"
#include "tickforge/order_book.hpp"

using namespace tf;

struct Recorder {
    std::vector<Trade> trades;
    void on_trade(const Trade& t) { trades.push_back(t); }
};

static void price_time_priority() {
    Recorder r;
    OrderBook<Recorder> b(r);
    b.add_limit(1, Side::Sell, 101, 10);
    b.add_limit(2, Side::Sell, 100, 10);  // better price
    b.add_limit(3, Side::Sell, 100, 10);  // same price, later
    auto out = b.add_limit(4, Side::Buy, 101, 25);
    CHECK(out.status == Status::Filled && out.filled == 25);
    CHECK(r.trades.size() == 3);
    CHECK(r.trades[0].maker == 2 && r.trades[0].price == 100 && r.trades[0].qty == 10);
    CHECK(r.trades[1].maker == 3 && r.trades[1].price == 100 && r.trades[1].qty == 10);
    CHECK(r.trades[2].maker == 1 && r.trades[2].price == 101 && r.trades[2].qty == 5);
    CHECK(b.best_ask() == 101 && !b.best_bid());
    CHECK(b.open_orders() == 1);
}

static void partial_fill_rests_remainder() {
    Recorder r;
    OrderBook<Recorder> b(r);
    b.add_limit(1, Side::Sell, 100, 10);
    auto out = b.add_limit(2, Side::Buy, 100, 30);
    CHECK(out.status == Status::Rested && out.filled == 10);
    CHECK(b.best_bid() == 100 && !b.best_ask());
    std::vector<LevelView> v;
    b.snapshot(Side::Buy, v);
    CHECK(v.size() == 1 && v[0] == (LevelView{100, 20, 1}));
}

static void ioc_and_fok() {
    Recorder r;
    OrderBook<Recorder> b(r);
    b.add_limit(1, Side::Sell, 100, 10);
    b.add_limit(2, Side::Sell, 101, 10);

    auto fok = b.add_limit(3, Side::Buy, 100, 15, Tif::FOK);  // only 10 available at <= 100
    CHECK(fok.status == Status::Rejected && r.trades.empty());

    auto ioc = b.add_limit(4, Side::Buy, 100, 15, Tif::IOC);
    CHECK(ioc.status == Status::Cancelled && ioc.filled == 10);
    CHECK(!b.best_bid());  // the IOC remainder never rests

    auto fok2 = b.add_limit(5, Side::Buy, 101, 10, Tif::FOK);
    CHECK(fok2.status == Status::Filled && b.open_orders() == 0);
}

static void market_order_sweeps() {
    Recorder r;
    OrderBook<Recorder> b(r);
    b.add_limit(1, Side::Buy, 99, 5);
    b.add_limit(2, Side::Buy, 98, 5);
    auto out = b.add_market(3, Side::Sell, 100);
    CHECK(out.status == Status::Cancelled && out.filled == 10);
    CHECK(r.trades.size() == 2 && r.trades[0].price == 99 && r.trades[1].price == 98);
    CHECK(b.open_orders() == 0 && b.levels(Side::Buy) == 0);
}

static void cancel_reduce_replace() {
    Recorder r;
    OrderBook<Recorder> b(r);
    b.add_limit(1, Side::Buy, 100, 10);
    b.add_limit(2, Side::Buy, 100, 10);
    b.add_limit(3, Side::Buy, 100, 10);

    CHECK(b.reduce(1, 4));  // keeps priority
    CHECK(b.cancel(2));
    CHECK(!b.cancel(2));
    CHECK(!b.cancel(999));
    CHECK(b.replace(3, 4, 100, 10).status == Status::Rested);  // goes to the back

    b.add_limit(5, Side::Buy, 100, 1);
    b.add_limit(6, Side::Sell, 100, 17);
    CHECK(r.trades.size() == 3);
    CHECK(r.trades[0].maker == 1 && r.trades[0].qty == 6);
    CHECK(r.trades[1].maker == 4 && r.trades[1].qty == 10);
    CHECK(r.trades[2].maker == 5 && r.trades[2].qty == 1);
    CHECK(b.open_orders() == 0);
}

static void rejects() {
    Recorder r;
    OrderBook<Recorder> b(r);
    CHECK(b.add_limit(1, Side::Buy, 100, 0).status == Status::Rejected);
    b.add_limit(1, Side::Buy, 100, 5);
    CHECK(b.add_limit(1, Side::Sell, 100, 5).status == Status::Rejected);  // duplicate id must not trade
    CHECK(r.trades.empty());
    b.add_limit(2, Side::Buy, 99, 5);
    CHECK(b.replace(1, 2, 101, 5).status == Status::Rejected);  // new id already live
    CHECK(b.best_bid() == 100);
}

static void levels_stay_sorted() {
    Recorder r;
    OrderBook<Recorder> b(r);
    const Price bids[] = {95, 99, 97, 98, 96, 99};
    const Price asks[] = {105, 101, 103, 102, 104, 101};
    OrderId id = 1;
    for (Price p : bids) b.add_limit(id++, Side::Buy, p, 1);
    for (Price p : asks) b.add_limit(id++, Side::Sell, p, 1);
    std::vector<LevelView> v;
    b.snapshot(Side::Buy, v);
    CHECK(v.size() == 5 && v[0] == (LevelView{99, 2, 2}) && v[4].price == 95);
    b.snapshot(Side::Sell, v, 2);
    CHECK(v.size() == 2 && v[0] == (LevelView{101, 2, 2}) && v[1].price == 102);
    b.cancel(3);  // 97, a middle level
    b.snapshot(Side::Buy, v);
    CHECK(v.size() == 4 && v[2].price == 96);
}

int main() {
    price_time_priority();
    partial_fill_rests_remainder();
    ioc_and_fok();
    market_order_sweeps();
    cancel_reduce_replace();
    rejects();
    levels_stay_sorted();
    std::puts("test_book: 7 scenarios passed");
}
