#include "net/udp_receiver.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#  include <ctime>  // struct timespec, used by linux/errqueue.h
#  include <linux/errqueue.h>
#  include <linux/net_tstamp.h>
#endif

namespace net {

struct UdpReceiver::Impl {
    alignas(64) std::array<std::array<uint8_t, kMaxPacket>, kBatch> bufs{};
    std::array<Packet, kBatch> packets{};
#if defined(__linux__)
    static constexpr std::size_t kCtrlLen = CMSG_SPACE(sizeof(struct scm_timestamping));
    struct alignas(alignof(cmsghdr)) CtrlBuf { uint8_t bytes[kCtrlLen]; };
    std::array<CtrlBuf, kBatch> ctrl{};
    std::array<struct iovec, kBatch>  iov{};
    std::array<struct mmsghdr, kBatch> msgs{};
#endif
};

namespace {

std::string sys_error(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

#if defined(__linux__)
uint64_t to_ns(const struct timespec& t) noexcept {
    return static_cast<uint64_t>(t.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(t.tv_nsec);
}
#endif

} // namespace

UdpReceiver::UdpReceiver() : impl_(std::make_unique<Impl>()) {}

UdpReceiver::~UdpReceiver() { close(); }

std::string UdpReceiver::open(const Options& opt) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) return sys_error("socket");
    std::string err = configure(opt);
    if (!err.empty()) close();  // never leave a half-configured socket open
    return err;
}

std::string UdpReceiver::configure(const Options& opt) {
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) return sys_error("fcntl");

    const int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#if defined(SO_REUSEPORT)
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    // Best effort: the kernel caps this at net.core.rmem_max.
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &opt.rcvbuf_bytes, sizeof opt.rcvbuf_bytes);

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(opt.port);
    struct in_addr group {};
    const bool multicast = !opt.group.empty();
    if (multicast) {
        if (::inet_pton(AF_INET, opt.group.c_str(), &group) != 1) return "invalid multicast group: " + opt.group;
        // Binding to the group address (not INADDR_ANY) filters out unrelated
        // traffic sent to the same port.
        addr.sin_addr = group;
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) return sys_error("bind");

    socklen_t alen = sizeof addr;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &alen) == 0) bound_port_ = ntohs(addr.sin_port);

    if (multicast) {
        struct ip_mreq mreq {};
        mreq.imr_multiaddr = group;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (!opt.interface_ip.empty() &&
            ::inet_pton(AF_INET, opt.interface_ip.c_str(), &mreq.imr_interface) != 1)
            return "invalid interface address: " + opt.interface_ip;
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0)
            return sys_error("IP_ADD_MEMBERSHIP");
    }

#if defined(__linux__)
    // Best effort: raising SO_BUSY_POLL needs CAP_NET_ADMIN on most kernels.
    // Without it the socket still works, just interrupt-driven; the global
    // net.core.busy_read sysctl is the alternative.
    if (opt.busy_poll_us > 0)
        ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &opt.busy_poll_us, sizeof opt.busy_poll_us);
    if (opt.timestamps) {
        const int ts = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                       SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        if (::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &ts, sizeof ts) != 0)
            return sys_error("SO_TIMESTAMPING");
    }
    for (std::size_t i = 0; i < kBatch; ++i) {
        impl_->iov[i] = {impl_->bufs[i].data(), kMaxPacket};
        auto& h = impl_->msgs[i].msg_hdr;
        h.msg_iov        = &impl_->iov[i];
        h.msg_iovlen     = 1;
        h.msg_control    = opt.timestamps ? impl_->ctrl[i].bytes : nullptr;
        h.msg_controllen = opt.timestamps ? Impl::kCtrlLen : 0;
    }
#else
    (void)opt.busy_poll_us;
    (void)opt.timestamps;
#endif
    return {};
}

std::size_t UdpReceiver::poll() noexcept {
    if (fd_ < 0) return 0;
#if defined(__linux__)
    for (auto& m : impl_->msgs) m.msg_hdr.msg_controllen = m.msg_hdr.msg_control ? Impl::kCtrlLen : 0;
    const int n = ::recvmmsg(fd_, impl_->msgs.data(), static_cast<unsigned>(kBatch), MSG_DONTWAIT, nullptr);
    if (n <= 0) return 0;
    for (int i = 0; i < n; ++i) {
        auto& m = impl_->msgs[static_cast<std::size_t>(i)];
        Packet& p = impl_->packets[static_cast<std::size_t>(i)];
        p = {impl_->bufs[static_cast<std::size_t>(i)].data(), m.msg_len, 0, false};
        for (cmsghdr* c = CMSG_FIRSTHDR(&m.msg_hdr); c != nullptr; c = CMSG_NXTHDR(&m.msg_hdr, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
                struct scm_timestamping ts;
                std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
                // ts[0] = software, ts[2] = raw hardware (zero when unavailable)
                if (ts.ts[2].tv_sec != 0 || ts.ts[2].tv_nsec != 0) {
                    p.rx_ns = to_ns(ts.ts[2]);
                    p.hw_timestamp = true;
                } else {
                    p.rx_ns = to_ns(ts.ts[0]);
                }
            }
        }
    }
    return static_cast<std::size_t>(n);
#else
    std::size_t n = 0;
    while (n < kBatch) {
        const ssize_t r = ::recv(fd_, impl_->bufs[n].data(), kMaxPacket, 0);
        if (r < 0) break;
        impl_->packets[n] = {impl_->bufs[n].data(), static_cast<std::size_t>(r), 0, false};
        ++n;
    }
    return n;
#endif
}

const UdpReceiver::Packet& UdpReceiver::packet(std::size_t i) const noexcept { return impl_->packets[i]; }

void UdpReceiver::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    bound_port_ = 0;
}

} // namespace net
