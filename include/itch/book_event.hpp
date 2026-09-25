#pragma once

#include <cstdint>

namespace itch {

enum class EventKind : uint8_t {
    kAdd     = 0,
    kExecute = 1,  ///< E and C
    kCancel  = 2,  ///< X (partial or full)
    kDelete  = 3,
    kReplace = 4,
};

/// Top-of-book change notification sent from the feed thread to consumers.
/// Exactly one 64-byte cache line, so a ring slot never straddles two lines.
struct alignas(64) BookEvent {
    uint64_t  timestamp;  ///< ITCH ns since midnight
    uint64_t  recv_tsc;   ///< local receive time in TSC ticks (0 in replay)
    uint64_t  ref;        ///< order reference number (new ref for replaces)
    uint64_t  bid_qty;    ///< top-of-book after the event
    uint64_t  ask_qty;
    uint32_t  price;      ///< event price, Price(4)
    uint32_t  shares;     ///< event size
    uint32_t  bid_price;  ///< top-of-book after the event (0 = side empty)
    uint32_t  ask_price;
    uint16_t  locate;
    EventKind kind;
    uint8_t   side;       ///< 'B' or 'S'
};

static_assert(sizeof(BookEvent) == 64, "BookEvent must be exactly one cache line");

} // namespace itch
