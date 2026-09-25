#include "itch/messages.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <vector>

namespace {

using namespace itch;

// Total message lengths from the TotalView-ITCH 5.0 specification (2023
// revision), written out independently of messages.hpp.
TEST(Messages, LengthTableMatchesSpec) {
    const std::map<char, uint16_t> spec = {
        {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
        {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
        {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
    for (int t = 0; t < 256; ++t) {
        const auto it = spec.find(static_cast<char>(t));
        const uint16_t expect = it == spec.end() ? 0 : it->second;
        EXPECT_EQ(kMessageLength[static_cast<std::size_t>(t)], expect) << "type " << t;
    }
}

// Hand-assembled wire bytes, not produced by encode().
TEST(Messages, DecodeAddOrderFromRawBytes) {
    const std::array<uint8_t, 36> wire = {
        'A',
        0x00, 0x2A,                          // locate 42
        0x00, 0x07,                          // tracking 7
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,  // timestamp 0x123456789ABC
        0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x42, 0x40,  // ref 1'000'000
        'S',
        0x00, 0x00, 0x01, 0x2C,              // shares 300
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',
        0x00, 0x1B, 0x77, 0x40};             // price 1'800'000 = $180.0000
    const auto m = decode<MsgAddOrder>(wire.data());
    EXPECT_EQ(m.locate, 42);
    EXPECT_EQ(m.tracking, 7);
    EXPECT_EQ(m.timestamp, 0x1234'5678'9ABCULL);
    EXPECT_EQ(m.ref, 1'000'000u);
    EXPECT_EQ(m.side, 'S');
    EXPECT_EQ(m.shares, 300u);
    EXPECT_EQ(trimmed(m.stock), "AAPL");
    EXPECT_EQ(m.price, 1'800'000u);
}

TEST(Messages, DecodeReplaceFromRawBytes) {
    std::array<uint8_t, 35> wire{};
    wire[0] = 'U';
    wire[2] = 0x05;                           // locate 5
    wire[10] = 0x01;                          // timestamp 1
    wire[18] = 0x10;                          // original ref 16
    wire[26] = 0x11;                          // new ref 17
    wire[29] = 0x03; wire[30] = 0xE8;         // shares 1000
    wire[31] = 0xFF; wire[32] = 0xFF; wire[33] = 0xFF; wire[34] = 0xFF;  // price max u32
    const auto m = decode<MsgOrderReplace>(wire.data());
    EXPECT_EQ(m.locate, 5);
    EXPECT_EQ(m.timestamp, 1u);
    EXPECT_EQ(m.original_ref, 16u);
    EXPECT_EQ(m.new_ref, 17u);
    EXPECT_EQ(m.shares, 1000u);
    EXPECT_EQ(m.price, 0xFFFF'FFFFu);
}

// Round-trip every message type with random field values.
template <class M>
class MessageRoundTrip : public ::testing::Test {};

template <class L> struct ToTypes;
template <class... Ms> struct ToTypes<TypeList<Ms...>> { using type = ::testing::Types<Ms...>; };
TYPED_TEST_SUITE(MessageRoundTrip, ToTypes<AllMessages>::type);

TYPED_TEST(MessageRoundTrip, EncodeDecodeIsIdentity) {
    using M = TypeParam;
    std::mt19937_64 rng(static_cast<uint64_t>(M::kType));
    for (int trial = 0; trial < 200; ++trial) {
        // Fill a wire image with random bytes (valid for every field type),
        // decode it, re-encode it, and demand byte-for-byte equality.
        std::vector<uint8_t> wire(M::kLength);
        for (auto& b : wire) b = static_cast<uint8_t>(rng());
        wire[0] = static_cast<uint8_t>(M::kType);
        const M m = decode<M>(wire.data());
        std::vector<uint8_t> again(M::kLength, 0xEE);
        ASSERT_EQ(encode(m, again.data()), M::kLength);
        ASSERT_EQ(again, wire);
    }
}

TEST(Messages, Load48OverreadMatchesPlainLoad) {
    std::mt19937_64 rng(5);
    std::array<uint8_t, 16> buf{};
    for (int i = 0; i < 1000; ++i) {
        for (auto& b : buf) b = static_cast<uint8_t>(rng());
        EXPECT_EQ(load_be48_overread(buf.data() + 5), load_be48(buf.data() + 5));
    }
}

} // namespace
