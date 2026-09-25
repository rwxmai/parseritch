#pragma once

/// AF_XDP kernel-bypass UDP receiver (Linux, libbpf + libxdp).
///
/// Frames are DMA'd (or, in copy mode, copied) by the kernel into a UMEM
/// region shared with user space; the program in bpf/xdp_udp_filter.bpf.c
/// redirects only the configured UDP flow to this socket, so the rest of the
/// interface's traffic keeps using the kernel stack. poll() returns the UDP
/// payloads of up to kBatch frames; they stay valid until the next poll(),
/// which hands the frames back to the kernel through the fill ring.
///
/// Compared with the v1 XdpSocket, this:
///   * loads and attaches its own filtering program instead of libbpf's
///     redirect-everything default;
///   * strips Ethernet/IPv4/UDP headers (net/packet.hpp) before the payload
///     reaches the MoldUDP64/ITCH parsers;
///   * returns each consumed frame to the fill ring by its own address;
///   * joins the multicast group, so the NIC and the switch deliver it.
///
/// Packet::rx_tsc is the TSC when poll() saw the batch (feed_handler stamps
/// events with it). NIC hardware timestamps through XDP RX metadata kfuncs
/// (Linux 6.3+) are not wired up.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace net {

class XdpReceiver {
public:
    static constexpr std::size_t kBatch = 64;

    struct Options {
        std::string ifname;            ///< e.g. "eth0"
        uint32_t    queue = 0;         ///< NIC rx queue to bind (steer the flow there with ethtool -N)
        uint16_t    dst_port = 0;      ///< UDP destination port to redirect
        std::string group;             ///< multicast group: filter on it and join it; empty = any dst
        std::string bpf_object;        ///< path to xdp_udp_filter.bpf.o
        bool        native_mode = false;  ///< driver-mode XDP (else generic/SKB mode)
        bool        zero_copy   = false;  ///< XDP_ZEROCOPY bind (needs driver support)
        uint32_t    frames      = 4096;   ///< UMEM frames (power of two)
        uint32_t    frame_size  = 2048;
    };

    struct Packet {
        const uint8_t* data = nullptr;  ///< UDP payload inside the UMEM frame
        std::size_t    len  = 0;
        uint64_t       rx_tsc = 0;
    };

    struct Stats {
        uint64_t frames      = 0;  ///< frames received on the socket
        uint64_t not_udp     = 0;  ///< dropped: failed Ethernet/IPv4/UDP validation
        uint64_t fill_starve = 0;  ///< polls where the fill ring had no room (0 with the 2x fill ring)
    };

    XdpReceiver();
    ~XdpReceiver();
    XdpReceiver(const XdpReceiver&)            = delete;
    XdpReceiver& operator=(const XdpReceiver&) = delete;

    /// Needs CAP_NET_ADMIN + CAP_BPF (or root). "" on success, else an error.
    [[nodiscard]] std::string open(const Options& opt);

    /// Non-blocking: returns frames of the previous poll to the kernel, then
    /// receives up to kBatch new ones. Read them with packet(i).
    std::size_t poll() noexcept;
    [[nodiscard]] const Packet& packet(std::size_t i) const noexcept;
    [[nodiscard]] const Stats& stats() const noexcept;

    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace net
