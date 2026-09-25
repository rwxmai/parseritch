#pragma once

/// Nasdaq TotalView-ITCH 5.0 message definitions (all 23 types).
///
/// Each message lists its fields once, as (member, wire offset) pairs. From
/// that one table we derive:
///   * decode()  wire -> struct (big-endian integers, Alpha fields copied)
///   * encode()  struct -> wire (used by tests, benchmarks and simulators)
///   * a static_assert that the fields tile the message with no gaps and
///     overlaps and end exactly at the spec's total length.
/// The offsets therefore can't drift from the lengths the parser validates
/// against.
///
/// Price(4) fields are uint32 (price x 10^4); Price(8) fields are uint64
/// (price x 10^8). Timestamps are nanoseconds since midnight. Alpha fields are
/// space-padded ASCII, left-justified.

#include "itch/bytes.hpp"
#include "itch/platform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace itch {

template <std::size_t N>
using Alpha = std::array<char, N>;

/// Symbol with trailing spaces trimmed.
template <std::size_t N>
constexpr std::string_view trimmed(const Alpha<N>& a) noexcept {
    std::size_t n = N;
    while (n > 0 && a[n - 1] == ' ') --n;
    return {a.data(), n};
}

/// Fields common to every message: type @0, locate @1, tracking @3, timestamp @5.
struct Header {
    uint16_t locate    = 0;
    uint16_t tracking  = 0;
    uint64_t timestamp = 0;  ///< ns since midnight (48-bit on the wire)
};
inline constexpr std::size_t kHeaderLength = 11;

template <auto Member, std::size_t Offset>
struct Field {
    static constexpr auto        member = Member;
    static constexpr std::size_t offset = Offset;
};

#define ITCH_MESSAGE(TYPE_CHAR, LENGTH) \
    static constexpr char        kType   = TYPE_CHAR; \
    static constexpr std::size_t kLength = LENGTH

struct MsgSystemEvent : Header {
    ITCH_MESSAGE('S', 12);
    char event_code = 0;
    static constexpr auto kFields = std::tuple{Field<&MsgSystemEvent::event_code, 11>{}};
};

struct MsgStockDirectory : Header {
    ITCH_MESSAGE('R', 39);
    Alpha<8> stock{};
    char     market_category = 0;
    char     financial_status = 0;
    uint32_t round_lot_size = 0;
    char     round_lots_only = 0;
    char     issue_classification = 0;
    Alpha<2> issue_subtype{};
    char     authenticity = 0;
    char     short_sale_threshold = 0;
    char     ipo_flag = 0;
    char     luld_ref_price_tier = 0;
    char     etp_flag = 0;
    uint32_t etp_leverage_factor = 0;
    char     inverse_indicator = 0;
    using M = MsgStockDirectory;
    static constexpr auto kFields = std::tuple{
        Field<&M::stock, 11>{}, Field<&M::market_category, 19>{}, Field<&M::financial_status, 20>{},
        Field<&M::round_lot_size, 21>{}, Field<&M::round_lots_only, 25>{},
        Field<&M::issue_classification, 26>{}, Field<&M::issue_subtype, 27>{},
        Field<&M::authenticity, 29>{}, Field<&M::short_sale_threshold, 30>{},
        Field<&M::ipo_flag, 31>{}, Field<&M::luld_ref_price_tier, 32>{}, Field<&M::etp_flag, 33>{},
        Field<&M::etp_leverage_factor, 34>{}, Field<&M::inverse_indicator, 38>{}};
};

struct MsgStockTradingAction : Header {
    ITCH_MESSAGE('H', 25);
    Alpha<8> stock{};
    char     trading_state = 0;
    char     reserved = 0;
    Alpha<4> reason{};
    using M = MsgStockTradingAction;
    static constexpr auto kFields = std::tuple{
        Field<&M::stock, 11>{}, Field<&M::trading_state, 19>{}, Field<&M::reserved, 20>{},
        Field<&M::reason, 21>{}};
};

struct MsgRegShoRestriction : Header {
    ITCH_MESSAGE('Y', 20);
    Alpha<8> stock{};
    char     reg_sho_action = 0;
    using M = MsgRegShoRestriction;
    static constexpr auto kFields = std::tuple{Field<&M::stock, 11>{}, Field<&M::reg_sho_action, 19>{}};
};

struct MsgMarketParticipantPosition : Header {
    ITCH_MESSAGE('L', 26);
    Alpha<4> mpid{};
    Alpha<8> stock{};
    char     primary_market_maker = 0;
    char     market_maker_mode = 0;
    char     market_participant_state = 0;
    using M = MsgMarketParticipantPosition;
    static constexpr auto kFields = std::tuple{
        Field<&M::mpid, 11>{}, Field<&M::stock, 15>{}, Field<&M::primary_market_maker, 23>{},
        Field<&M::market_maker_mode, 24>{}, Field<&M::market_participant_state, 25>{}};
};

struct MsgMwcbDeclineLevel : Header {
    ITCH_MESSAGE('V', 35);
    uint64_t level1 = 0;  ///< Price(8)
    uint64_t level2 = 0;
    uint64_t level3 = 0;
    using M = MsgMwcbDeclineLevel;
    static constexpr auto kFields = std::tuple{
        Field<&M::level1, 11>{}, Field<&M::level2, 19>{}, Field<&M::level3, 27>{}};
};

struct MsgMwcbStatus : Header {
    ITCH_MESSAGE('W', 12);
    char breached_level = 0;
    static constexpr auto kFields = std::tuple{Field<&MsgMwcbStatus::breached_level, 11>{}};
};

struct MsgIpoQuotingPeriodUpdate : Header {
    ITCH_MESSAGE('K', 28);
    Alpha<8> stock{};
    uint32_t release_time = 0;  ///< seconds since midnight
    char     release_qualifier = 0;
    uint32_t ipo_price = 0;
    using M = MsgIpoQuotingPeriodUpdate;
    static constexpr auto kFields = std::tuple{
        Field<&M::stock, 11>{}, Field<&M::release_time, 19>{}, Field<&M::release_qualifier, 23>{},
        Field<&M::ipo_price, 24>{}};
};

struct MsgLuldAuctionCollar : Header {
    ITCH_MESSAGE('J', 35);
    Alpha<8> stock{};
    uint32_t reference_price = 0;
    uint32_t upper_price = 0;
    uint32_t lower_price = 0;
    uint32_t extension = 0;
    using M = MsgLuldAuctionCollar;
    static constexpr auto kFields = std::tuple{
        Field<&M::stock, 11>{}, Field<&M::reference_price, 19>{}, Field<&M::upper_price, 23>{},
        Field<&M::lower_price, 27>{}, Field<&M::extension, 31>{}};
};

struct MsgOperationalHalt : Header {
    ITCH_MESSAGE('h', 21);
    Alpha<8> stock{};
    char     market_code = 0;
    char     halt_action = 0;
    using M = MsgOperationalHalt;
    static constexpr auto kFields = std::tuple{
        Field<&M::stock, 11>{}, Field<&M::market_code, 19>{}, Field<&M::halt_action, 20>{}};
};

struct MsgAddOrder : Header {
    ITCH_MESSAGE('A', 36);
    uint64_t ref = 0;
    char     side = 0;  ///< 'B' or 'S'
    uint32_t shares = 0;
    Alpha<8> stock{};
    uint32_t price = 0;
    using M = MsgAddOrder;
    static constexpr auto kFields = std::tuple{
        Field<&M::ref, 11>{}, Field<&M::side, 19>{}, Field<&M::shares, 20>{},
        Field<&M::stock, 24>{}, Field<&M::price, 32>{}};
};

struct MsgAddOrderMpid : Header {
    ITCH_MESSAGE('F', 40);
    uint64_t ref = 0;
    char     side = 0;
    uint32_t shares = 0;
    Alpha<8> stock{};
    uint32_t price = 0;
    Alpha<4> attribution{};
    using M = MsgAddOrderMpid;
    static constexpr auto kFields = std::tuple{
        Field<&M::ref, 11>{}, Field<&M::side, 19>{}, Field<&M::shares, 20>{},
        Field<&M::stock, 24>{}, Field<&M::price, 32>{}, Field<&M::attribution, 36>{}};
};

struct MsgOrderExecuted : Header {
    ITCH_MESSAGE('E', 31);
    uint64_t ref = 0;
    uint32_t executed_shares = 0;
    uint64_t match_number = 0;
    using M = MsgOrderExecuted;
    static constexpr auto kFields = std::tuple{
        Field<&M::ref, 11>{}, Field<&M::executed_shares, 19>{}, Field<&M::match_number, 23>{}};
};

struct MsgOrderExecutedWithPrice : Header {
    ITCH_MESSAGE('C', 36);
    uint64_t ref = 0;
    uint32_t executed_shares = 0;
    uint64_t match_number = 0;
    char     printable = 0;
    uint32_t execution_price = 0;
    using M = MsgOrderExecutedWithPrice;
    static constexpr auto kFields = std::tuple{
        Field<&M::ref, 11>{}, Field<&M::executed_shares, 19>{}, Field<&M::match_number, 23>{},
        Field<&M::printable, 31>{}, Field<&M::execution_price, 32>{}};
};

struct MsgOrderCancel : Header {
    ITCH_MESSAGE('X', 23);
    uint64_t ref = 0;
    uint32_t cancelled_shares = 0;
    using M = MsgOrderCancel;
    static constexpr auto kFields = std::tuple{Field<&M::ref, 11>{}, Field<&M::cancelled_shares, 19>{}};
};

struct MsgOrderDelete : Header {
    ITCH_MESSAGE('D', 19);
    uint64_t ref = 0;
    static constexpr auto kFields = std::tuple{Field<&MsgOrderDelete::ref, 11>{}};
};

struct MsgOrderReplace : Header {
    ITCH_MESSAGE('U', 35);
    uint64_t original_ref = 0;
    uint64_t new_ref = 0;
    uint32_t shares = 0;
    uint32_t price = 0;
    using M = MsgOrderReplace;
    static constexpr auto kFields = std::tuple{
        Field<&M::original_ref, 11>{}, Field<&M::new_ref, 19>{}, Field<&M::shares, 27>{},
        Field<&M::price, 31>{}};
};

/// Non-cross trade against a non-displayed order. Per the spec, `ref` has been
/// 0 since 2010 and `side` has always been 'B' since 2014.
struct MsgTrade : Header {
    ITCH_MESSAGE('P', 44);
    uint64_t ref = 0;
    char     side = 0;
    uint32_t shares = 0;
    Alpha<8> stock{};
    uint32_t price = 0;
    uint64_t match_number = 0;
    using M = MsgTrade;
    static constexpr auto kFields = std::tuple{
        Field<&M::ref, 11>{}, Field<&M::side, 19>{}, Field<&M::shares, 20>{},
        Field<&M::stock, 24>{}, Field<&M::price, 32>{}, Field<&M::match_number, 36>{}};
};

struct MsgCrossTrade : Header {
    ITCH_MESSAGE('Q', 40);
    uint64_t shares = 0;
    Alpha<8> stock{};
    uint32_t cross_price = 0;
    uint64_t match_number = 0;
    char     cross_type = 0;
    using M = MsgCrossTrade;
    static constexpr auto kFields = std::tuple{
        Field<&M::shares, 11>{}, Field<&M::stock, 19>{}, Field<&M::cross_price, 27>{},
        Field<&M::match_number, 31>{}, Field<&M::cross_type, 39>{}};
};

struct MsgBrokenTrade : Header {
    ITCH_MESSAGE('B', 19);
    uint64_t match_number = 0;
    static constexpr auto kFields = std::tuple{Field<&MsgBrokenTrade::match_number, 11>{}};
};

/// Net Order Imbalance Indicator.
struct MsgNoii : Header {
    ITCH_MESSAGE('I', 50);
    uint64_t paired_shares = 0;
    uint64_t imbalance_shares = 0;
    char     imbalance_direction = 0;
    Alpha<8> stock{};
    uint32_t far_price = 0;
    uint32_t near_price = 0;
    uint32_t current_reference_price = 0;
    char     cross_type = 0;
    char     price_variation_indicator = 0;
    using M = MsgNoii;
    static constexpr auto kFields = std::tuple{
        Field<&M::paired_shares, 11>{}, Field<&M::imbalance_shares, 19>{},
        Field<&M::imbalance_direction, 27>{}, Field<&M::stock, 28>{}, Field<&M::far_price, 36>{},
        Field<&M::near_price, 40>{}, Field<&M::current_reference_price, 44>{},
        Field<&M::cross_type, 48>{}, Field<&M::price_variation_indicator, 49>{}};
};

/// Retail Price Improvement Indicator.
struct MsgRetailInterest : Header {
    ITCH_MESSAGE('N', 20);
    Alpha<8> stock{};
    char     interest_flag = 0;
    using M = MsgRetailInterest;
    static constexpr auto kFields = std::tuple{Field<&M::stock, 11>{}, Field<&M::interest_flag, 19>{}};
};

/// Direct Listing with Capital Raise Price Discovery.
struct MsgDirectListing : Header {
    ITCH_MESSAGE('O', 48);
    Alpha<8> stock{};
    char     open_eligibility_status = 0;
    uint32_t min_allowable_price = 0;
    uint32_t max_allowable_price = 0;
    uint32_t near_execution_price = 0;
    uint64_t near_execution_time = 0;
    uint32_t lower_price_range_collar = 0;
    uint32_t upper_price_range_collar = 0;
    using M = MsgDirectListing;
    static constexpr auto kFields = std::tuple{
        Field<&M::stock, 11>{}, Field<&M::open_eligibility_status, 19>{},
        Field<&M::min_allowable_price, 20>{}, Field<&M::max_allowable_price, 24>{},
        Field<&M::near_execution_price, 28>{}, Field<&M::near_execution_time, 32>{},
        Field<&M::lower_price_range_collar, 40>{}, Field<&M::upper_price_range_collar, 44>{}};
};

#undef ITCH_MESSAGE

template <class... Ms>
struct TypeList {};

using AllMessages = TypeList<
    MsgSystemEvent, MsgStockDirectory, MsgStockTradingAction, MsgRegShoRestriction,
    MsgMarketParticipantPosition, MsgMwcbDeclineLevel, MsgMwcbStatus, MsgIpoQuotingPeriodUpdate,
    MsgLuldAuctionCollar, MsgOperationalHalt, MsgAddOrder, MsgAddOrderMpid, MsgOrderExecuted,
    MsgOrderExecutedWithPrice, MsgOrderCancel, MsgOrderDelete, MsgOrderReplace, MsgTrade,
    MsgCrossTrade, MsgBrokenTrade, MsgNoii, MsgRetailInterest, MsgDirectListing>;

// ---------------------------------------------------------------------------
// Generic field codec
// ---------------------------------------------------------------------------

namespace detail {

template <class M, auto Member>
using member_t = std::remove_cvref_t<decltype(std::declval<M&>().*Member)>;

template <class T>
inline constexpr bool is_alpha_v = false;
template <std::size_t N>
inline constexpr bool is_alpha_v<Alpha<N>> = true;

template <class T>
ITCH_ALWAYS_INLINE void decode_value(const uint8_t* p, T& out) noexcept {
    if constexpr (std::is_same_v<T, char>)          out = static_cast<char>(*p);
    else if constexpr (std::is_same_v<T, uint16_t>) out = load_be16(p);
    else if constexpr (std::is_same_v<T, uint32_t>) out = load_be32(p);
    else if constexpr (std::is_same_v<T, uint64_t>) out = load_be64(p);
    else if constexpr (is_alpha_v<T>)               std::memcpy(out.data(), p, out.size());
    else static_assert(sizeof(T) == 0, "unsupported ITCH field type");
}

template <class T>
ITCH_ALWAYS_INLINE void encode_value(uint8_t* p, const T& v) noexcept {
    if constexpr (std::is_same_v<T, char>)          *p = static_cast<uint8_t>(v);
    else if constexpr (std::is_same_v<T, uint16_t>) store_be16(p, v);
    else if constexpr (std::is_same_v<T, uint32_t>) store_be32(p, v);
    else if constexpr (std::is_same_v<T, uint64_t>) store_be64(p, v);
    else if constexpr (is_alpha_v<T>)               std::memcpy(p, v.data(), v.size());
    else static_assert(sizeof(T) == 0, "unsupported ITCH field type");
}

/// True when the fields start right after the header, are contiguous, and
/// end exactly at kLength.
template <class M>
consteval bool fields_tile_message() {
    return std::apply(
        [](auto... f) {
            std::size_t next = kHeaderLength;
            bool ok = true;
            ((ok = ok && decltype(f)::offset == next,
              next = decltype(f)::offset + sizeof(member_t<M, decltype(f)::member>)), ...);
            return ok && next == M::kLength;
        },
        M::kFields);
}

} // namespace detail

/// Decode message M from `p` (p[0] is the type byte). Precondition: the
/// buffer holds M::kLength bytes.
template <class M>
[[nodiscard]] ITCH_ALWAYS_INLINE M decode(const uint8_t* p) noexcept {
    static_assert(detail::fields_tile_message<M>(), "field offsets must tile the message exactly");
    M m;
    m.locate    = load_be16(p + 1);
    m.tracking  = load_be16(p + 3);
    m.timestamp = load_be48_overread(p + 5);
    std::apply([&](auto... f) { (detail::decode_value(p + decltype(f)::offset, m.*decltype(f)::member), ...); },
               M::kFields);
    return m;
}

/// Encode m into `out` (M::kLength bytes, type byte included). Returns the length.
template <class M>
std::size_t encode(const M& m, uint8_t* out) noexcept {
    static_assert(detail::fields_tile_message<M>(), "field offsets must tile the message exactly");
    out[0] = static_cast<uint8_t>(M::kType);
    store_be16(out + 1, m.locate);
    store_be16(out + 3, m.tracking);
    store_be48(out + 5, m.timestamp);
    std::apply([&](auto... f) { (detail::encode_value(out + decltype(f)::offset, m.*decltype(f)::member), ...); },
               M::kFields);
    return M::kLength;
}

/// Expected total length per type byte; 0 for types ITCH 5.0 does not define.
inline constexpr std::array<uint16_t, 256> kMessageLength = [] {
    std::array<uint16_t, 256> t{};
    [&]<class... Ms>(TypeList<Ms...>) {
        ((t[static_cast<uint8_t>(Ms::kType)] = static_cast<uint16_t>(Ms::kLength)), ...);
    }(AllMessages{});
    return t;
}();

} // namespace itch
