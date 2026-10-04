#pragma once
// A deliberately simple, obviously-correct order book built from node-based
// standard containers. It is the oracle for differential testing and the
// baseline the benchmark compares against.
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "tickforge/types.hpp"

namespace tf {

template <class Handler>
class ReferenceBook {
public:
    explicit ReferenceBook(Handler& h, std::size_t = 0) : handler_(h) {}

    AddOutcome add_limit(OrderId id, Side side, Price price, Qty qty, Tif tif = Tif::GTC) {
        if (qty == 0 || index_.count(id)) return {Status::Rejected, 0};
        if (tif == Tif::FOK && available(side, price) < qty) return {Status::Rejected, 0};
        Qty left = qty;
        if (side == Side::Buy) left = match(asks_, id, side, price, left);
        else left = match(bids_, id, side, price, left);
        if (left == 0) return {Status::Filled, qty};
        if (tif != Tif::GTC) return {Status::Cancelled, qty - left};
        if (side == Side::Buy) rest(bids_, id, side, price, left);
        else rest(asks_, id, side, price, left);
        return {Status::Rested, qty - left};
    }

    AddOutcome add_market(OrderId id, Side side, Qty qty) {
        Price p = side == Side::Buy ? std::numeric_limits<Price>::max()
                                    : std::numeric_limits<Price>::min();
        return add_limit(id, side, p, qty, Tif::IOC);
    }

    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        Ref r = it->second;
        if (r.side == Side::Buy) remove(bids_, r);
        else remove(asks_, r);
        index_.erase(it);
        return true;
    }

    bool reduce(OrderId id, Qty by) {
        auto it = index_.find(id);
        if (it == index_.end() || by == 0) return false;
        Ref r = it->second;
        if (by >= r.it->qty) return cancel(id);
        r.it->qty -= by;
        if (r.side == Side::Buy) bids_[r.price].total -= by;
        else asks_[r.price].total -= by;
        return true;
    }

    AddOutcome replace(OrderId id, OrderId new_id, Price price, Qty qty) {
        auto it = index_.find(id);
        if (it == index_.end() || qty == 0) return {Status::Rejected, 0};
        if (new_id != id && index_.count(new_id)) return {Status::Rejected, 0};
        Side side = it->second.side;
        cancel(id);
        return add_limit(new_id, side, price, qty);
    }

    void prefetch_id(OrderId) const {}     // node-based containers have nothing useful to hint
    void prefetch_order(OrderId) const {}

    bool add_resting(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0 || index_.count(id)) return false;
        if (side == Side::Buy) rest(bids_, id, side, price, qty);
        else rest(asks_, id, side, price, qty);
        return true;
    }

    Exec execute(OrderId id, Qty qty) {
        auto it = index_.find(id);
        if (it == index_.end()) return Exec::Unknown;
        const Qty have = it->second.it->qty;
        if (qty < have) {
            reduce(id, qty);
            return Exec::Partial;
        }
        cancel(id);
        return qty == have ? Exec::Full : Exec::Over;
    }

    bool replace_resting(OrderId id, OrderId new_id, Price price, Qty qty) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        const Side side = it->second.side;
        cancel(id);
        return add_resting(new_id, side, price, qty);
    }

    std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
    }
    std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
    }
    std::size_t open_orders() const { return index_.size(); }
    std::size_t levels(Side s) const { return s == Side::Buy ? bids_.size() : asks_.size(); }

    void snapshot(Side s, std::vector<LevelView>& out,
                  std::size_t max_levels = std::numeric_limits<std::size_t>::max()) const {
        out.clear();
        auto fill = [&](const auto& m) {
            for (const auto& [p, l] : m) {
                if (out.size() >= max_levels) break;
                out.push_back({p, l.total, static_cast<std::uint32_t>(l.q.size())});
            }
        };
        if (s == Side::Buy) fill(bids_);
        else fill(asks_);
    }

private:
    struct RefOrder {
        OrderId id;
        Qty qty;
    };
    struct RefLevel {
        std::list<RefOrder> q;
        Qty total = 0;
    };
    struct Ref {
        Side side;
        Price price;
        std::list<RefOrder>::iterator it;
    };

    static bool crosses(Side taker, Price tp, Price mp) {
        return taker == Side::Buy ? tp >= mp : tp <= mp;
    }

    Qty available(Side side, Price price) const {
        Qty a = 0;
        if (side == Side::Buy) {
            for (const auto& [p, l] : asks_) {
                if (!crosses(side, price, p)) break;
                a += l.total;
            }
        } else {
            for (const auto& [p, l] : bids_) {
                if (!crosses(side, price, p)) break;
                a += l.total;
            }
        }
        return a;
    }

    template <class Map>
    Qty match(Map& opp, OrderId taker, Side side, Price price, Qty qty) {
        while (qty && !opp.empty()) {
            auto lit = opp.begin();
            if (!crosses(side, price, lit->first)) break;
            RefLevel& l = lit->second;
            while (qty && !l.q.empty()) {
                RefOrder& m = l.q.front();
                Qty fill = qty < m.qty ? qty : m.qty;
                m.qty -= fill;
                l.total -= fill;
                qty -= fill;
                handler_.on_trade(Trade{m.id, taker, lit->first, fill});
                if (m.qty == 0) {
                    index_.erase(m.id);
                    l.q.pop_front();
                }
            }
            if (l.q.empty()) opp.erase(lit);
        }
        return qty;
    }

    template <class Map>
    void rest(Map& own, OrderId id, Side side, Price price, Qty qty) {
        RefLevel& l = own[price];
        l.q.push_back({id, qty});
        l.total += qty;
        index_[id] = Ref{side, price, std::prev(l.q.end())};
    }

    template <class Map>
    void remove(Map& own, const Ref& r) {
        auto lit = own.find(r.price);
        lit->second.total -= r.it->qty;
        lit->second.q.erase(r.it);
        if (lit->second.q.empty()) own.erase(lit);
    }

    Handler& handler_;
    std::map<Price, RefLevel, std::greater<Price>> bids_;
    std::map<Price, RefLevel> asks_;
    std::unordered_map<OrderId, Ref> index_;
};

}  // namespace tf
