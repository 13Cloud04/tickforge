#pragma once
#include <cstdint>

namespace tf {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks; no floating point anywhere on the hot path
using Qty = std::uint64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

// Time in force.
enum class Tif : std::uint8_t {
    GTC,  // rest whatever does not match
    IOC,  // match what you can, drop the remainder
    FOK,  // match everything immediately or do nothing
};

enum class Status : std::uint8_t {
    Rested,     // some or all of the order is now resting in the book
    Filled,     // fully matched
    Cancelled,  // IOC/market remainder dropped (filled may be > 0)
    Rejected,   // duplicate id, zero quantity, or FOK that could not fully fill
};

struct AddOutcome {
    Status status;
    Qty filled;
};

// Outcome of applying an exchange-reported execution or partial cancel to a resting order.
enum class Exec : std::uint8_t {
    Unknown,  // no such order
    Partial,  // order reduced, still resting
    Full,     // order fully consumed and removed
    Over,     // more than the remaining quantity was reported; order removed
};

struct Trade {
    OrderId maker;
    OrderId taker;
    Price price;  // always the maker's (resting) price
    Qty qty;
};

struct LevelView {
    Price price;
    Qty qty;
    std::uint32_t orders;
    bool operator==(const LevelView&) const = default;
};

inline constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

}  // namespace tf
