#include "net/xdp_receiver.hpp"

#include "itch/clock.hpp"
#include "net/packet.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xdp/xsk.h>

namespace net {

struct XdpReceiver::Impl {
    Options opt;
    int     ifindex = 0;
    uint32_t attach_flags = 0;

    // BPF program and maps
    bpf_object* obj = nullptr;
    int prog_fd = -1;
    bool attached = false;

    // UMEM and rings
    void*        umem_area = nullptr;
    std::size_t  umem_size = 0;
    xsk_umem*    umem = nullptr;
    xsk_socket*  xsk = nullptr;
    xsk_ring_prod fill{};
    xsk_ring_cons comp{};
    xsk_ring_cons rx{};

    // Frames handed out by the last poll(), returned on the next one.
    std::array<uint64_t, kBatch> pending{};
    std::size_t pending_n = 0;

    std::array<Packet, kBatch> packets{};
    Stats stats{};
    int mcast_fd = -1;  // regular socket holding the multicast membership
};

namespace {

std::string err(const char* what, int e) { return std::string(what) + ": " + std::strerror(e); }

} // namespace

XdpReceiver::XdpReceiver() : impl_(std::make_unique<Impl>()) {}
XdpReceiver::~XdpReceiver() { close(); }

std::string XdpReceiver::open(const Options& opt) {
    close();
    Impl& s = *impl_;
    s.opt = opt;
    if ((opt.frames & (opt.frames - 1)) != 0 || opt.frames == 0) return "frames must be a power of two";

    s.ifindex = static_cast<int>(::if_nametoindex(opt.ifname.c_str()));
    if (s.ifindex == 0) return err(("if_nametoindex(" + opt.ifname + ")").c_str(), errno);

    // --- BPF program: load, configure the filter, attach -------------------
    s.obj = bpf_object__open_file(opt.bpf_object.c_str(), nullptr);
    if (s.obj == nullptr) return err(("bpf_object__open_file(" + opt.bpf_object + ")").c_str(), errno);
    if (int e = bpf_object__load(s.obj); e != 0) return err("bpf_object__load", -e);
    bpf_program* prog = bpf_object__find_program_by_name(s.obj, "xdp_udp_filter");
    bpf_map* cfg_map = bpf_object__find_map_by_name(s.obj, "cfg_map");
    bpf_map* xsks_map = bpf_object__find_map_by_name(s.obj, "xsks_map");
    if (prog == nullptr || cfg_map == nullptr || xsks_map == nullptr) return "BPF object lacks xdp_udp_filter/cfg_map/xsks_map";
    s.prog_fd = bpf_program__fd(prog);

    struct {
        uint32_t dst_ip;
        uint16_t dst_port;
        uint16_t pad;
    } cfg{0, htons(opt.dst_port), 0};
    in_addr group{};
    if (!opt.group.empty()) {
        if (::inet_pton(AF_INET, opt.group.c_str(), &group) != 1) return "invalid group: " + opt.group;
        cfg.dst_ip = group.s_addr;  // already network byte order
    }
    const uint32_t zero = 0;
    if (bpf_map_update_elem(bpf_map__fd(cfg_map), &zero, &cfg, BPF_ANY) != 0) return err("cfg_map update", errno);

    // --- UMEM ---------------------------------------------------------------
    s.umem_size = std::size_t{opt.frames} * opt.frame_size;
    s.umem_area = ::mmap(nullptr, s.umem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (s.umem_area == MAP_FAILED) { s.umem_area = nullptr; return err("mmap(UMEM)", errno); }
    xsk_umem_config ucfg{};
    // Twice the frame count: the kernel can publish new rx entries before it
    // advances the fill ring's consumer index, so frames just received may
    // briefly have no free fill slot to go back into with a 1x ring.
    ucfg.fill_size      = opt.frames * 2;
    ucfg.comp_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    ucfg.frame_size     = opt.frame_size;
    ucfg.frame_headroom = 0;
    if (int e = xsk_umem__create(&s.umem, s.umem_area, s.umem_size, &s.fill, &s.comp, &ucfg); e != 0)
        return err("xsk_umem__create", -e);

    // --- socket (our program, not libxdp's default) ---------------------------
    xsk_socket_config xcfg{};
    xcfg.rx_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xcfg.tx_size      = 0;
    xcfg.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;
    xcfg.bind_flags   = static_cast<uint16_t>(XDP_USE_NEED_WAKEUP | (opt.zero_copy ? XDP_ZEROCOPY : XDP_COPY));
    if (int e = xsk_socket__create(&s.xsk, opt.ifname.c_str(), opt.queue, s.umem, &s.rx, nullptr, &xcfg); e != 0)
        return err("xsk_socket__create", -e);

    const int xsk_fd = xsk_socket__fd(s.xsk);
    if (bpf_map_update_elem(bpf_map__fd(xsks_map), &opt.queue, &xsk_fd, BPF_ANY) != 0) return err("xsks_map update", errno);

    // Hand every frame to the kernel.
    uint32_t idx = 0;
    if (xsk_ring_prod__reserve(&s.fill, opt.frames, &idx) != opt.frames) return "fill ring smaller than UMEM";
    for (uint32_t i = 0; i < opt.frames; ++i)
        *xsk_ring_prod__fill_addr(&s.fill, idx++) = uint64_t{i} * opt.frame_size;
    xsk_ring_prod__submit(&s.fill, opt.frames);

    s.attach_flags = opt.native_mode ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;
    if (int e = bpf_xdp_attach(s.ifindex, s.prog_fd, s.attach_flags, nullptr); e != 0) return err("bpf_xdp_attach", -e);
    s.attached = true;

    // Join the group on an ordinary socket: this programs the NIC's multicast
    // filter and sends the IGMP report; AF_XDP alone does neither.
    if (!opt.group.empty()) {
        s.mcast_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        ip_mreqn mreq{};
        mreq.imr_multiaddr = group;
        mreq.imr_ifindex = s.ifindex;
        if (s.mcast_fd < 0 || ::setsockopt(s.mcast_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0)
            return err("IP_ADD_MEMBERSHIP", errno);
    }
    return {};
}

std::size_t XdpReceiver::poll() noexcept {
    Impl& s = *impl_;
    if (s.xsk == nullptr) return 0;

    // 1. Return last batch's frames to the kernel.
    if (s.pending_n != 0) {
        uint32_t idx = 0;
        const auto n = static_cast<uint32_t>(s.pending_n);
        if (xsk_ring_prod__reserve(&s.fill, n, &idx) == n) {
            for (std::size_t i = 0; i < s.pending_n; ++i) *xsk_ring_prod__fill_addr(&s.fill, idx++) = s.pending[i];
            xsk_ring_prod__submit(&s.fill, n);
            s.pending_n = 0;
        } else {
            ++s.stats.fill_starve;  // keep them; retry next poll
            return 0;
        }
    }
    if (xsk_ring_prod__needs_wakeup(&s.fill))
        ::recvfrom(xsk_socket__fd(s.xsk), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);

    // 2. Receive.
    uint32_t idx = 0;
    const uint32_t rcvd = xsk_ring_cons__peek(&s.rx, kBatch, &idx);
    if (rcvd == 0) return 0;
    const uint64_t now = itch::TscClock::now();
    std::size_t out = 0;
    for (uint32_t i = 0; i < rcvd; ++i) {
        const xdp_desc* d = xsk_ring_cons__rx_desc(&s.rx, idx++);
        s.pending[s.pending_n++] = xsk_umem__extract_addr(d->addr);  // frame address, for the fill ring
        const auto* frame = static_cast<const uint8_t*>(xsk_umem__get_data(s.umem_area, xsk_umem__add_offset_to_addr(d->addr)));
        UdpDatagram dg;
        if (parse_udp_frame(frame, d->len, dg) != FrameError::kOk) {
            ++s.stats.not_udp;
            continue;
        }
        s.packets[out++] = {dg.payload, dg.len, now};
    }
    xsk_ring_cons__release(&s.rx, rcvd);
    s.stats.frames += rcvd;
    return out;
}

const XdpReceiver::Packet& XdpReceiver::packet(std::size_t i) const noexcept { return impl_->packets[i]; }
const XdpReceiver::Stats& XdpReceiver::stats() const noexcept { return impl_->stats; }

void XdpReceiver::close() noexcept {
    Impl& s = *impl_;
    if (s.attached) {
        bpf_xdp_detach(s.ifindex, s.attach_flags, nullptr);
        s.attached = false;
    }
    if (s.mcast_fd >= 0) { ::close(s.mcast_fd); s.mcast_fd = -1; }
    if (s.xsk != nullptr) { xsk_socket__delete(s.xsk); s.xsk = nullptr; }
    if (s.umem != nullptr) { xsk_umem__delete(s.umem); s.umem = nullptr; }
    if (s.umem_area != nullptr) { ::munmap(s.umem_area, s.umem_size); s.umem_area = nullptr; }
    if (s.obj != nullptr) { bpf_object__close(s.obj); s.obj = nullptr; }
    s.prog_fd = -1;
    s.pending_n = 0;
}

} // namespace net
