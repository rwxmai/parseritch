#pragma once

/// Parser handler that maintains order books and forwards events to a sink.
///
///   Parser<BookBuilder<Sink>>  ->  BookEngine  ->  Sink
///
/// For every book-changing message the builder applies the event and then calls
///   sink.on_book(const MsgX&, const BookUpdate&)   if the sink declares it.
/// Any other message is forwarded as
///   sink.on(const MsgX&)                          if the sink declares it,
/// and is otherwise neither decoded nor forwarded.
///
/// If the sink declares  bool wants(uint8_t type, uint16_t locate) const,
/// the builder exposes it to the Parser, and rejected messages are neither
/// decoded nor applied. Filter by locate, not by order-message type: every
/// ITCH message carries its stock locate (a Replace keeps its original's),
/// so a locate filter keeps each wanted book complete, while dropping, say,
/// Deletes would leave stale orders behind.

#include "itch/bytes.hpp"
#include "itch/messages.hpp"
#include "itch/order_book.hpp"
#include "itch/platform.hpp"

#include <concepts>
#include <type_traits>

namespace itch {

template <class M>
inline constexpr bool is_book_message_v =
    std::is_same_v<M, MsgAddOrder> || std::is_same_v<M, MsgAddOrderMpid> ||
    std::is_same_v<M, MsgOrderExecuted> || std::is_same_v<M, MsgOrderExecutedWithPrice> ||
    std::is_same_v<M, MsgOrderCancel> || std::is_same_v<M, MsgOrderDelete> ||
    std::is_same_v<M, MsgOrderReplace>;

template <class Sink, class M>
concept SinkOnBook = requires(Sink& s, const M& m, const BookUpdate& u) { s.on_book(m, u); };

template <class Sink, class M>
concept SinkOn = requires(Sink& s, const M& m) { s.on(m); };

template <class Sink>
concept SinkWants = requires(const Sink& s, uint8_t type, uint16_t locate) {
    { s.wants(type, locate) } -> std::convertible_to<bool>;
};

template <class Sink>
class BookBuilder {
public:
    BookBuilder(BookEngine& engine, Sink& sink) noexcept : engine_(engine), sink_(sink) {}

    ITCH_ALWAYS_INLINE void on(const MsgAddOrder& m) {
        notify(m, engine_.add(m.locate, m.ref, static_cast<uint8_t>(m.side), m.shares, m.price));
    }
    ITCH_ALWAYS_INLINE void on(const MsgAddOrderMpid& m) {
        notify(m, engine_.add(m.locate, m.ref, static_cast<uint8_t>(m.side), m.shares, m.price));
    }
    ITCH_ALWAYS_INLINE void on(const MsgOrderExecuted& m) {
        notify(m, engine_.reduce(m.ref, m.executed_shares));
    }
    ITCH_ALWAYS_INLINE void on(const MsgOrderExecutedWithPrice& m) {
        notify(m, engine_.reduce(m.ref, m.executed_shares));
    }
    ITCH_ALWAYS_INLINE void on(const MsgOrderCancel& m) {
        notify(m, engine_.reduce(m.ref, m.cancelled_shares));
    }
    ITCH_ALWAYS_INLINE void on(const MsgOrderDelete& m) {
        notify(m, engine_.remove(m.ref));
    }
    ITCH_ALWAYS_INLINE void on(const MsgOrderReplace& m) {
        notify(m, engine_.replace(m.original_ref, m.new_ref, m.shares, m.price));
    }

    /// The stock directory arrives before trading starts, so this is the point
    /// to allocate level storage for every listed symbol, keeping allocation
    /// off the path of the first order.
    void on(const MsgStockDirectory& m) {
        engine_.prepare(m.locate);
        if constexpr (SinkOn<Sink, MsgStockDirectory>) sink_.on(m);
    }

    template <class M>
        requires(!is_book_message_v<M> && !std::is_same_v<M, MsgStockDirectory> && SinkOn<Sink, M>)
    ITCH_ALWAYS_INLINE void on(const M& m) {
        sink_.on(m);
    }

    /// Present only when the sink filters; see the file comment.
    [[nodiscard]] ITCH_ALWAYS_INLINE bool wants(uint8_t type, uint16_t locate) const
        requires SinkWants<Sink>
    {
        return sink_.wants(type, locate);
    }

    /// Lookahead hint from Parser::parse_stream_prefetch for a record that
    /// will be parsed a few records from now. The record is not validated
    /// yet, so only read fields the length proves are there.
    ITCH_ALWAYS_INLINE void prefetch(const uint8_t* msg, std::size_t len) const noexcept {
        if (len < 19) return;  // shortest order message ('D') is 19 bytes
        if constexpr (SinkWants<Sink>) {
            if (!sink_.wants(msg[0], load_be16(msg + 1))) return;
        }
        switch (msg[0]) {
            case 'A': case 'F':
                engine_.prefetch_order(load_be64(msg + 11));  // group the insert lands in
                if (len >= 20) engine_.prefetch_levels(load_be16(msg + 1), msg[19]);  // side byte
                break;
            case 'E': case 'C': case 'X': case 'D':
                engine_.prefetch_order(load_be64(msg + 11));
                engine_.prefetch_levels(load_be16(msg + 1));
                break;
            case 'U':
                engine_.prefetch_order(load_be64(msg + 11));
                if (len >= 27) engine_.prefetch_order(load_be64(msg + 19));
                engine_.prefetch_levels(load_be16(msg + 1));
                break;
            default:
                break;
        }
    }

    [[nodiscard]] BookEngine& engine() noexcept { return engine_; }
    [[nodiscard]] Sink& sink() noexcept { return sink_; }

private:
    template <class M>
    ITCH_ALWAYS_INLINE void notify(const M& m, const BookUpdate& u) {
        if constexpr (SinkOnBook<Sink, M>) sink_.on_book(m, u);
    }

    BookEngine& engine_;
    Sink&       sink_;
};

/// A sink that ignores everything, for pure book-building.
struct NullSink {};

} // namespace itch
