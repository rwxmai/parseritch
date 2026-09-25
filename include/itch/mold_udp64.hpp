#pragma once

/// MoldUDP64 framing (Nasdaq MoldUDP64 Protocol Specification v1.00).
///
///   Header (20 bytes, big-endian):
///     Session          @0   10  alphanumeric
///     Sequence Number  @10   8  sequence number of the first message
///     Message Count    @18   2  0 = heartbeat, 0xFFFF = end of session
///   Then Message Count blocks of [u16 length][message], numbered
///   consecutively from Sequence Number.
///
/// The old live path passed the raw UDP payload straight to the ITCH stream
/// parser, so the session bytes were read as message lengths. MoldSequencer
/// also does gap detection and duplicate suppression, so the A and B lines of
/// a redundant feed can both be fed into it: whichever packet arrives first is
/// delivered and the copy is dropped.

#include "itch/bytes.hpp"
#include "itch/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itch {

inline constexpr std::size_t kMoldHeaderLength = 20;
inline constexpr uint16_t    kMoldHeartbeat    = 0;
inline constexpr uint16_t    kMoldEndOfSession = 0xFFFF;

struct MoldHeader {
    Alpha<10> session{};
    uint64_t  sequence = 0;
    uint16_t  count    = 0;
};

/// Parse the header. Returns false if the packet is shorter than 20 bytes.
[[nodiscard]] inline bool parse_mold_header(const uint8_t* pkt, std::size_t len,
                                            MoldHeader& out) noexcept {
    if (len < kMoldHeaderLength) return false;
    std::memcpy(out.session.data(), pkt, 10);
    out.sequence = load_be64(pkt + 10);
    out.count    = load_be16(pkt + 18);
    return true;
}

/// Tracks the expected sequence number of one session.
class MoldSequencer {
public:
    struct Result {
        uint64_t delivered = 0;
        uint64_t gap_first = 0;  ///< first missing sequence number (valid if gap_count)
        uint64_t gap_count = 0;  ///< messages lost before this packet
        bool     malformed = false;
        bool     end_of_session = false;
        bool     session_changed = false;
    };

    /// Process one packet and call deliver(msg, len, seq) for each message not
    /// seen before, in order. A gap is reported and then skipped (recovery
    /// needs a retransmission request, which is outside this class).
    template <class Deliver>
    Result on_packet(const uint8_t* pkt, std::size_t len, Deliver&& deliver) {
        Result r;
        MoldHeader h;
        if (!parse_mold_header(pkt, len, h)) { r.malformed = true; return r; }
        if (h.count == kMoldEndOfSession) { r.end_of_session = true; return r; }
        // First packet, or a new session (e.g. the next trading day, or a
        // restarted server): sequence numbers restart, so re-baseline instead
        // of treating everything as a duplicate of the old session.
        if (next_ == 0 || h.session != session_) {
            r.session_changed = next_ != 0;
            session_ = h.session;
            next_ = h.sequence;
        }
        // A heartbeat's sequence number is the next expected message.
        if (h.count == kMoldHeartbeat) {
            if (h.sequence > next_) { r.gap_first = next_; r.gap_count = h.sequence - next_; next_ = h.sequence; }
            return r;
        }
        if (h.sequence > next_) {
            r.gap_first = next_;
            r.gap_count = h.sequence - next_;
            next_ = h.sequence;
        }
        std::size_t off = kMoldHeaderLength;
        for (uint64_t i = 0; i < h.count; ++i) {
            if (off + 2 > len) { r.malformed = true; return r; }
            const std::size_t n = load_be16(pkt + off);
            if (off + 2 + n > len) { r.malformed = true; return r; }
            const uint64_t seq = h.sequence + i;
            if (seq == next_) {
                deliver(pkt + off + 2, n, seq);
                ++next_;
                ++r.delivered;
            }
            off += 2 + n;
        }
        return r;
    }

    [[nodiscard]] uint64_t next_expected() const noexcept { return next_; }

    [[nodiscard]] const Alpha<10>& session() const noexcept { return session_; }

private:
    Alpha<10> session_{};
    uint64_t  next_ = 0;
};

} // namespace itch
