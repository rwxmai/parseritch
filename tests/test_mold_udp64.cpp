#include "itch/mold_udp64.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace itch;

/// Packet whose messages are single bytes equal to (sequence & 0xFF).
std::vector<uint8_t> packet(uint64_t seq, uint16_t count, const char* session = "SESSION001") {
    std::vector<uint8_t> p(kMoldHeaderLength);
    std::memcpy(p.data(), session, 10);
    store_be64(p.data() + 10, seq);
    store_be16(p.data() + 18, count);
    if (count == kMoldEndOfSession) return p;
    for (uint16_t i = 0; i < count; ++i) {
        p.push_back(0);
        p.push_back(1);
        p.push_back(static_cast<uint8_t>(seq + i));
    }
    return p;
}

struct Collector {
    std::vector<uint64_t> seqs;
    MoldSequencer seq;
    MoldSequencer::Result feed(const std::vector<uint8_t>& p) {
        return seq.on_packet(p.data(), p.size(), [&](const uint8_t* m, std::size_t len, uint64_t s) {
            EXPECT_EQ(len, 1u);
            EXPECT_EQ(m[0], static_cast<uint8_t>(s));
            seqs.push_back(s);
        });
    }
};

TEST(MoldUdp64, HeaderLayout) {
    const auto p = packet(0x0102'0304'0506'0708ULL, 3);
    MoldHeader h;
    ASSERT_TRUE(parse_mold_header(p.data(), p.size(), h));
    EXPECT_EQ(std::string(h.session.data(), 10), "SESSION001");
    EXPECT_EQ(h.sequence, 0x0102'0304'0506'0708ULL);
    EXPECT_EQ(h.count, 3);
    EXPECT_FALSE(parse_mold_header(p.data(), 19, h));
}

TEST(MoldUdp64, InOrderDelivery) {
    Collector c;
    c.feed(packet(1, 3));
    c.feed(packet(4, 2));
    EXPECT_EQ(c.seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(c.seq.next_expected(), 6u);
}

// A/B line arbitration: the same packets arrive twice and each message is
// delivered once, from whichever copy came first.
TEST(MoldUdp64, DuplicatesFromSecondLineAreDropped) {
    Collector c;
    c.feed(packet(1, 3));   // line A
    c.feed(packet(1, 3));   // line B, same packet
    c.feed(packet(4, 2));   // line B first this time
    c.feed(packet(4, 2));   // line A late
    EXPECT_EQ(c.seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
}

TEST(MoldUdp64, PartialOverlapDeliversOnlyNewMessages) {
    Collector c;
    c.feed(packet(1, 3));
    c.feed(packet(2, 4));  // 2..5, of which 4 and 5 are new
    EXPECT_EQ(c.seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
}

TEST(MoldUdp64, GapIsReported) {
    Collector c;
    c.feed(packet(1, 2));
    const auto r = c.feed(packet(10, 1));
    EXPECT_EQ(r.gap_first, 3u);
    EXPECT_EQ(r.gap_count, 7u);
    EXPECT_EQ(c.seqs.back(), 10u);
}

TEST(MoldUdp64, HeartbeatAdvancesExpectationAndRevealsGaps) {
    Collector c;
    c.feed(packet(1, 2));
    const auto r = c.feed(packet(5, kMoldHeartbeat));
    EXPECT_EQ(r.gap_count, 2u);
    EXPECT_EQ(c.seq.next_expected(), 5u);
}

TEST(MoldUdp64, NewSessionRebaselinesSequence) {
    Collector c;
    c.feed(packet(1, 3));
    c.feed(packet(4, 2));
    const auto r = c.feed(packet(1, 2, "SESSION002"));  // sequence restarts
    EXPECT_TRUE(r.session_changed);
    EXPECT_EQ(r.delivered, 2u);
    EXPECT_EQ(c.seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5, 1, 2}));
}

TEST(MoldUdp64, EndOfSessionAndMalformed) {
    Collector c;
    EXPECT_TRUE(c.feed(packet(1, kMoldEndOfSession)).end_of_session);
    auto bad = packet(1, 3);
    bad.resize(bad.size() - 2);
    EXPECT_TRUE(c.feed(bad).malformed);
    EXPECT_TRUE(c.feed(std::vector<uint8_t>(5)).malformed);
}

} // namespace
