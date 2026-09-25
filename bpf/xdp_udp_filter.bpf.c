/* XDP program: redirect the market-data UDP flow to an AF_XDP socket and pass
 * everything else to the normal kernel stack.
 *
 * The v1 AF_XDP path loaded no program of its own. libbpf's default program
 * redirects *every* packet on the queue to the socket, taking SSH, ARP and
 * everything else with it. This program redirects only IPv4/UDP datagrams
 * whose destination port (and, if configured, destination address) match
 * cfg_map; fragments and all other traffic go to XDP_PASS.
 *
 * Built with: clang -O2 -g -target bpf -c xdp_udp_filter.bpf.c
 * Only non-GPL-only helpers are used (map lookup, redirect_map), so the
 * object carries no license section.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct filter_cfg {
    __u32 dst_ip;    /* network byte order; 0 = any destination */
    __u16 dst_port;  /* network byte order */
    __u16 pad;
};

/* Written by user space before attaching (XdpReceiver::open). */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct filter_cfg);
} cfg_map SEC(".maps");

/* rx queue index -> AF_XDP socket fd */
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

struct vlan_hdr {
    __be16 tci;
    __be16 encapsulated_proto;
};

SEC("xdp")
int xdp_udp_filter(struct xdp_md *ctx)
{
    void *data = (void *)(long)ctx->data;
    void *end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > end)
        return XDP_PASS;
    __be16 proto = eth->h_proto;
    void *l3 = eth + 1;
    if (proto == bpf_htons(ETH_P_8021Q) || proto == bpf_htons(ETH_P_8021AD)) {
        struct vlan_hdr *vlan = l3;
        if ((void *)(vlan + 1) > end)
            return XDP_PASS;
        proto = vlan->encapsulated_proto;
        l3 = vlan + 1;
    }
    if (proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *ip = l3;
    if ((void *)(ip + 1) > end)
        return XDP_PASS;
    if (ip->ihl < 5 || ip->protocol != IPPROTO_UDP)
        return XDP_PASS;
    if (ip->frag_off & bpf_htons(0x3FFF)) /* MF flag or fragment offset */
        return XDP_PASS;

    struct udphdr *udp = (void *)ip + ip->ihl * 4;
    if ((void *)(udp + 1) > end)
        return XDP_PASS;

    __u32 key = 0;
    struct filter_cfg *cfg = bpf_map_lookup_elem(&cfg_map, &key);
    if (!cfg || udp->dest != cfg->dst_port)
        return XDP_PASS;
    if (cfg->dst_ip && ip->daddr != cfg->dst_ip)
        return XDP_PASS;

    /* No socket bound to this queue -> fall back to the stack. */
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}
