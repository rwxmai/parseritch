#pragma once

/// Batched UDP receiver for multicast market data.
///
/// Linux path: recvmmsg() drains up to kBatch datagrams per syscall;
/// SO_BUSY_POLL spins in the driver instead of sleeping on an interrupt; and
/// SO_TIMESTAMPING returns a kernel (and, if the NIC is set up for it, a
/// hardware) receive timestamp per packet in the control-message buffer.
/// Other POSIX systems fall back to one recvmsg() per datagram without
/// timestamps. That fallback exists so the unit tests run on a developer
/// machine.
///
/// Hardware timestamps also need the NIC's RX filter enabled (SIOCSHWTSTAMP,
/// e.g. `hwstamp_ctl -i eth0 -r 1`, as root). Without it the kernel software
/// timestamp is reported and Packet::hw_timestamp is false.
///
/// This replaces the previous AF_XDP socket, which:
///   * never loaded a BPF program, so libbpf's default program would redirect
///     all traffic on the NIC queue (not just the feed) into the socket;
///   * handed Ethernet/IP/UDP-framed packets to the ITCH parser;
///   * refilled the fill ring with the wrong frame addresses;
///   * never joined the multicast group.
/// A correct AF_XDP path needs a Linux box with a supported NIC to test on;
/// see the README roadmap.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace net {

class UdpReceiver {
public:
    static constexpr std::size_t kBatch     = 64;
    static constexpr std::size_t kMaxPacket = 2048;  // > 1500-byte Ethernet MTU

    struct Options {
        std::string group;        ///< multicast group; empty = unicast
        uint16_t    port = 0;
        std::string interface_ip; ///< local interface address for the join; empty = default
        int         busy_poll_us = 50;
        int         rcvbuf_bytes = 32 << 20;
        bool        timestamps   = true;
    };

    struct Packet {
        const uint8_t* data = nullptr;
        std::size_t    len  = 0;
        uint64_t       rx_ns = 0;          ///< CLOCK_REALTIME ns; 0 if unavailable
        bool           hw_timestamp = false;
    };

    UdpReceiver();
    ~UdpReceiver();
    UdpReceiver(const UdpReceiver&)            = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    /// Returns an empty string on success, otherwise an error description.
    [[nodiscard]] std::string open(const Options& opt);

    /// Non-blocking: receive up to kBatch datagrams. Returns how many arrived;
    /// read them with packet(i) until the next poll().
    std::size_t poll() noexcept;

    [[nodiscard]] const Packet& packet(std::size_t i) const noexcept;

    /// Locally bound port (useful when opened with port 0).
    [[nodiscard]] uint16_t bound_port() const noexcept { return bound_port_; }

    void close() noexcept;

private:
    std::string configure(const Options& opt);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    int      fd_ = -1;
    uint16_t bound_port_ = 0;
};

} // namespace net
