#pragma once
// Decoder for NASDAQ TotalView-ITCH 5.0, the exchange's full order-by-order
// feed. A capture file is a sequence of [2-byte big-endian length][message].
// Every message starts with: type (1), stock locate (2), tracking number (2),
// timestamp in nanoseconds since midnight (6). All integers are big-endian.
//
// Only the messages that change the visible book are decoded into fields;
// the rest are counted and skipped.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tf::itch {

inline std::uint16_t be16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
inline std::uint32_t be32(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
inline std::uint64_t be64(const unsigned char* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}
inline std::uint64_t be48(const unsigned char* p) { return (static_cast<std::uint64_t>(be16(p)) << 32) | be32(p + 2); }

struct Header {
    char type;
    std::uint16_t locate;      // per-day integer id of the instrument
    std::uint64_t timestamp;   // ns since midnight
};

inline Header header(const unsigned char* m) { return {static_cast<char>(m[0]), be16(m + 1), be48(m + 5)}; }

// Minimum valid length of each message this decoder reads fields from.
inline std::size_t min_length(char type) {
    switch (type) {
        case 'S': return 12;  // system event
        case 'R': return 39;  // stock directory
        case 'A': return 36;  // add order
        case 'F': return 40;  // add order with attribution
        case 'E': return 31;  // order executed
        case 'C': return 36;  // order executed with price
        case 'X': return 23;  // order cancel (partial)
        case 'D': return 19;  // order delete
        case 'U': return 35;  // order replace
        default: return 11;
    }
}

// Calls handler.on_message(const unsigned char* msg, std::size_t len) for each
// complete message. Returns the number of bytes consumed; a truncated final
// message (the capture was cut mid-stream) is left unconsumed.
template <class Handler>
std::size_t for_each_message(const unsigned char* data, std::size_t size, Handler& handler) {
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::size_t len = be16(data + pos);
        if (len == 0 || pos + 2 + len > size) break;
        handler.on_message(data + pos + 2, len);
        pos += 2 + len;
    }
    return pos;
}

// As for_each_message, but with two extra cursors running ahead of the one
// being processed: handler.hint_far(msg) is called `far` messages early and
// handler.hint_near(msg) `near` messages early, so the handler can start
// memory loads for a message before its turn comes.
template <class Handler>
std::size_t for_each_message_ahead(const unsigned char* data, std::size_t size, Handler& handler,
                                   unsigned far = 16, unsigned near = 8) {
    auto next = [&](std::size_t pos) -> std::size_t {
        if (pos + 2 > size) return size + 1;
        const std::size_t len = be16(data + pos);
        return (len == 0 || pos + 2 + len > size) ? size + 1 : pos + 2 + len;
    };
    std::size_t pos = 0, a = 0, b = 0;
    for (unsigned i = 0; i < far && a <= size; ++i) {
        const std::size_t n = next(a);
        if (n > size) break;
        handler.hint_far(data + a + 2);
        if (i >= far - near) {
            handler.hint_near(data + b + 2);
            b = next(b);
        }
        a = n;
    }
    for (;;) {
        const std::size_t end = next(pos);
        if (end > size) break;
        if (const std::size_t na = next(a); na <= size) {
            handler.hint_far(data + a + 2);
            a = na;
        }
        if (b < a) {
            handler.hint_near(data + b + 2);
            b = next(b);
        }
        handler.on_message(data + pos + 2, be16(data + pos));
        pos = end;
    }
    return pos;
}

}  // namespace tf::itch
