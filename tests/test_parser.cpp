#include "itch/parser.hpp"
#include "itch/symbol_directory.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace itch;

struct Recorder {
    std::vector<uint64_t> adds;
    std::vector<uint64_t> deletes;
    void on(const MsgAddOrder& m) { adds.push_back(m.ref); }
    void on(const MsgOrderDelete& m) { deletes.push_back(m.ref); }
};

template <class M>
void append(std::vector<uint8_t>& out, const M& m) {
    const std::size_t at = out.size();
    out.resize(at + 2 + M::kLength);
    store_be16(out.data() + at, static_cast<uint16_t>(M::kLength));
    encode(m, out.data() + at + 2);
}

void append_raw(std::vector<uint8_t>& out, std::vector<uint8_t> body) {
    const std::size_t at = out.size();
    out.resize(at + 2);
    store_be16(out.data() + at, static_cast<uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

MsgAddOrder add(uint64_t ref) {
    MsgAddOrder m;
    m.ref = ref; m.side = 'B'; m.shares = 1; m.price = 1;
    return m;
}

TEST(Parser, DispatchesOnlyHandledTypes) {
    std::vector<uint8_t> s;
    append(s, add(1));
    MsgSystemEvent se; se.event_code = 'O';
    append(s, se);  // not handled by Recorder: validated, counted, not decoded
    MsgOrderDelete d; d.ref = 1;
    append(s, d);
    Recorder r;
    Parser<Recorder> p(r);
    EXPECT_EQ(p.parse_stream(s.data(), s.size()), s.size());
    EXPECT_EQ(r.adds, std::vector<uint64_t>{1});
    EXPECT_EQ(r.deletes, std::vector<uint64_t>{1});
    EXPECT_EQ(p.stats().messages, 3u);
    EXPECT_EQ(p.stats().by_type['S'], 1u);
}

// The old parser advanced one extra byte after a zero-length record and lost
// framing for the rest of the buffer.
TEST(Parser, ZeroLengthRecordDoesNotDesynchronise) {
    std::vector<uint8_t> s;
    append(s, add(1));
    append_raw(s, {});
    append(s, add(2));
    Recorder r;
    Parser<Recorder> p(r);
    p.parse_stream(s.data(), s.size());
    EXPECT_EQ(r.adds, (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(p.stats().empty, 1u);
}

TEST(Parser, WrongLengthIsRejectedBeforeDecoding) {
    std::vector<uint8_t> s;
    append_raw(s, {'A', 0, 1, 0, 0});  // 'A' needs 36 bytes; decoding would over-read
    append(s, add(3));
    Recorder r;
    Parser<Recorder> p(r);
    p.parse_stream(s.data(), s.size());
    EXPECT_EQ(r.adds, std::vector<uint64_t>{3});
    EXPECT_EQ(p.stats().bad_length, 1u);
}

TEST(Parser, UnknownTypeIsCounted) {
    std::vector<uint8_t> s;
    append_raw(s, {'Z', 1, 2, 3});
    Recorder r;
    Parser<Recorder> p(r);
    p.parse_stream(s.data(), s.size());
    EXPECT_EQ(p.stats().unknown_type, 1u);
    EXPECT_EQ(p.stats().messages, 0u);
}

TEST(Parser, TrailingPartialRecordIsNotConsumed) {
    std::vector<uint8_t> s;
    append(s, add(1));
    const std::size_t complete = s.size();
    append(s, add(2));
    s.resize(s.size() - 5);  // truncate the second record
    Recorder r;
    Parser<Recorder> p(r);
    EXPECT_EQ(p.parse_stream(s.data(), s.size()), complete);
    EXPECT_EQ(r.adds, std::vector<uint64_t>{1});
}

TEST(SymbolDirectory, KnownAndUnknownLocates) {
    SymbolDirectory d;
    EXPECT_EQ(d.symbol(42), "");  // unknown locate is empty, not 8 NULs
    MsgStockDirectory m;
    m.locate = 42;
    m.stock = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    d.on(m);
    EXPECT_EQ(d.symbol(42), "AAPL");
    EXPECT_EQ(d.locate("AAPL"), std::optional<uint16_t>(42));
    EXPECT_FALSE(d.locate("MSFT").has_value());
}

} // namespace
