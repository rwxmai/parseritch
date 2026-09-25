#include "net/packet.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using net::FrameError;

/// Build an Ethernet/IPv4/UDP frame by hand (independent of the parser).
std::vector<uint8_t> frame(const std::vector<uint8_t>& payload, bool vlan = false,
                           uint8_t ihl_words = 5, uint16_t frag = 0x4000 /* DF */,
                           uint8_t proto = 17, std::size_t eth_pad = 0) {
    std::vector<uint8_t> f;
    const auto put16 = [&](uint16_t v) { f.push_back(uint8_t(v >> 8)); f.push_back(uint8_t(v)); };
    const auto put32 = [&](uint32_t v) { put16(uint16_t(v >> 16)); put16(uint16_t(v)); };
    for (int i = 0; i < 6; ++i) f.push_back(0x01);  // dst MAC (multicast)
    for (int i = 0; i < 6; ++i) f.push_back(0x02);  // src MAC
    if (vlan) { put16(0x8100); put16(0x0064); }
    put16(0x0800);
    const std::size_t ihl = std::size_t{ihl_words} * 4;
    const auto ip_total = static_cast<uint16_t>(ihl + 8 + payload.size());
    f.push_back(uint8_t(0x40 | ihl_words));
    f.push_back(0);
    put16(ip_total);
    put16(0x1234);                // id
    put16(frag);
    f.push_back(64);              // ttl
    f.push_back(proto);
    put16(0);                     // checksum (not verified)
    put32(0x0A000001);            // 10.0.0.1
    put32(0xE9360C6F);            // 233.54.12.111
    for (std::size_t i = 20; i < ihl; ++i) f.push_back(0x01);  // IP options (NOPs)
    put16(40000);                 // src port
    put16(26477);                 // dst port
    put16(static_cast<uint16_t>(8 + payload.size()));
    put16(0);                     // udp checksum
    f.insert(f.end(), payload.begin(), payload.end());
    f.insert(f.end(), eth_pad, 0);
    return f;
}

TEST(Packet, PlainVlanOptionsAndPadding) {
    const std::vector<uint8_t> payload = {'M', 'O', 'L', 'D'};
    for (bool vlan : {false, true}) {
        for (uint8_t ihl : {uint8_t{5}, uint8_t{7}}) {
            const auto f = frame(payload, vlan, ihl, 0x4000, 17, /*eth_pad=*/20);
            net::UdpDatagram d;
            ASSERT_EQ(net::parse_udp_frame(f.data(), f.size(), d), FrameError::kOk);
            ASSERT_EQ(d.len, payload.size()) << "padding must not leak into the payload";
            EXPECT_EQ(std::vector<uint8_t>(d.payload, d.payload + d.len), payload);
            EXPECT_EQ(d.dst_ip, 0xE9360C6Fu);
            EXPECT_EQ(d.src_ip, 0x0A000001u);
            EXPECT_EQ(d.dst_port, 26477);
            EXPECT_EQ(d.src_port, 40000);
        }
    }
}

TEST(Packet, RejectsEveryMalformation) {
    const std::vector<uint8_t> payload(10, 0xAB);
    net::UdpDatagram d;
    const auto good = frame(payload);
    // Truncation at every length shorter than the full frame.
    for (std::size_t n = 0; n < good.size(); ++n)
        EXPECT_NE(net::parse_udp_frame(good.data(), n, d), FrameError::kOk) << "len " << n;

    auto arp = good;
    arp[12] = 0x08; arp[13] = 0x06;
    EXPECT_EQ(net::parse_udp_frame(arp.data(), arp.size(), d), FrameError::kNotIpv4);

    auto v6 = good;
    v6[14] = 0x65;  // version 6
    EXPECT_EQ(net::parse_udp_frame(v6.data(), v6.size(), d), FrameError::kBadIpHeader);

    const auto more_frags = frame(payload, false, 5, 0x2000);
    EXPECT_EQ(net::parse_udp_frame(more_frags.data(), more_frags.size(), d), FrameError::kFragment);
    const auto offset_frag = frame(payload, false, 5, 0x0010);
    EXPECT_EQ(net::parse_udp_frame(offset_frag.data(), offset_frag.size(), d), FrameError::kFragment);

    const auto tcp = frame(payload, false, 5, 0x4000, 6);
    EXPECT_EQ(net::parse_udp_frame(tcp.data(), tcp.size(), d), FrameError::kNotUdp);

    auto long_udp = good;
    long_udp[14 + 20 + 4] = 0x01;  // UDP length > IP payload
    EXPECT_EQ(net::parse_udp_frame(long_udp.data(), long_udp.size(), d), FrameError::kBadUdpLength);
}

} // namespace
