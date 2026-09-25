#pragma once

/// Ethernet / IPv4 / UDP frame parsing for kernel-bypass receive.
///
/// An AF_XDP socket hands over whole Ethernet frames, not UDP payloads. (The
/// v1 XDP path passed frames straight to the ITCH parser, headers included.)
/// This parser validates each layer and returns a view of the UDP payload:
///
///   Ethernet II, optionally one 802.1Q / 802.1ad VLAN tag
///   IPv4: version 4, IHL >= 5 (options skipped), not fragmented
///   UDP:  length consistent with the IP total length
///
/// Checksums are not verified: NICs validate them (and multicast market-data
/// UDP checksums are commonly zero). IPv4 fragments are rejected rather than
/// reassembled: MoldUDP64 packets fit in one frame, so a fragment means a
/// misconfigured path, not data to rebuild.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace net {

struct UdpDatagram {
    const uint8_t* payload = nullptr;
    std::size_t    len     = 0;
    uint32_t       src_ip  = 0;  ///< host byte order
    uint32_t       dst_ip  = 0;  ///< host byte order
    uint16_t       src_port = 0;
    uint16_t       dst_port = 0;
};

enum class FrameError : uint8_t {
    kOk,
    kTruncated,     ///< shorter than the headers it claims to carry
    kNotIpv4,       ///< EtherType is not IPv4 (ARP, IPv6, ...)
    kBadIpHeader,   ///< version != 4 or IHL < 5
    kFragment,      ///< IPv4 fragment (MF set or non-zero offset)
    kNotUdp,
    kBadUdpLength,
};

namespace detail {
inline uint16_t be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t be32(const uint8_t* p) noexcept {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
} // namespace detail

[[nodiscard]] inline FrameError parse_udp_frame(const uint8_t* frame, std::size_t len,
                                                UdpDatagram& out) noexcept {
    using detail::be16;
    using detail::be32;
    constexpr std::size_t kEth = 14, kVlan = 4, kIpMin = 20, kUdp = 8;

    if (len < kEth) return FrameError::kTruncated;
    std::size_t off = 12;
    uint16_t ethertype = be16(frame + off);
    if (ethertype == 0x8100 || ethertype == 0x88A8) {  // one VLAN tag
        if (len < kEth + kVlan) return FrameError::kTruncated;
        off += kVlan;
        ethertype = be16(frame + off);
    }
    if (ethertype != 0x0800) return FrameError::kNotIpv4;
    off += 2;  // start of the IPv4 header

    if (len < off + kIpMin) return FrameError::kTruncated;
    const uint8_t* ip = frame + off;
    const unsigned version = ip[0] >> 4;
    const std::size_t ihl = std::size_t{ip[0] & 0x0Fu} * 4;
    if (version != 4 || ihl < kIpMin) return FrameError::kBadIpHeader;
    const std::size_t ip_total = be16(ip + 2);
    // Frames may carry Ethernet padding after the IP datagram, never less.
    if (ip_total < ihl || len < off + ip_total) return FrameError::kTruncated;
    const uint16_t frag = be16(ip + 6);
    if ((frag & 0x2000u) != 0 || (frag & 0x1FFFu) != 0) return FrameError::kFragment;
    if (ip[9] != 17) return FrameError::kNotUdp;

    if (ip_total < ihl + kUdp) return FrameError::kTruncated;
    const uint8_t* udp = ip + ihl;
    const std::size_t udp_len = be16(udp + 4);
    if (udp_len < kUdp || udp_len > ip_total - ihl) return FrameError::kBadUdpLength;

    out.payload  = udp + kUdp;
    out.len      = udp_len - kUdp;
    out.src_ip   = be32(ip + 12);
    out.dst_ip   = be32(ip + 16);
    out.src_port = be16(udp);
    out.dst_port = be16(udp + 2);
    return FrameError::kOk;
}

} // namespace net
