#!/usr/bin/env python3
"""Replay a Nasdaq ITCH binary file as a MoldUDP64 feed over UDP.

    tools/mold_send.py FILE HOST PORT [--pps N] [--session NAME] [--mtu BYTES]

HOST may be a unicast or a multicast address. Messages are packed into
MoldUDP64 packets (20-byte header: session, sequence of the first message,
message count; then [u16 length][message] blocks) up to --mtu bytes of UDP
payload, sent at up to --pps packets per second, and followed by
end-of-session packets (message count 0xFFFF), which make
`feed_handler --mcast/--port` exit and print its report.

Useful for exercising the live receive paths (UDP socket and AF_XDP)
without an exchange connection. Standard library only.
"""

import argparse
import socket
import struct
import sys
import time


def records(path):
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    while off + 2 <= len(data):
        (n,) = struct.unpack_from(">H", data, off)
        if off + 2 + n > len(data):
            break
        yield data[off : off + 2 + n]  # keep the length prefix: Mold uses the same framing
        off += 2 + n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("--pps", type=float, default=20000, help="max packets per second (0 = unpaced)")
    ap.add_argument("--session", default="ITCHSYNTH1", help="10-character session name")
    ap.add_argument("--mtu", type=int, default=1400, help="max UDP payload bytes per packet")
    ap.add_argument("--ttl", type=int, default=1, help="multicast TTL")
    args = ap.parse_args()

    session = args.session.encode("ascii")[:10].ljust(10, b" ")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, args.ttl)
    dest = (args.host, args.port)

    seq = 1
    batch, batch_bytes, sent_packets, sent_msgs = [], 0, 0, 0
    interval = 1.0 / args.pps if args.pps > 0 else 0.0
    next_send = time.perf_counter()

    def flush():
        nonlocal seq, batch, batch_bytes, sent_packets, sent_msgs, next_send
        if not batch:
            return
        if interval:
            now = time.perf_counter()
            if now < next_send:
                time.sleep(next_send - now)
            next_send = max(now, next_send) + interval
        sock.sendto(session + struct.pack(">QH", seq, len(batch)) + b"".join(batch), dest)
        seq += len(batch)
        sent_packets += 1
        sent_msgs += len(batch)
        batch, batch_bytes = [], 0

    for rec in records(args.file):
        if batch_bytes + len(rec) > args.mtu - 20 or len(batch) == 0xFFFE:
            flush()
        batch.append(rec)
        batch_bytes += len(rec)
    flush()

    for _ in range(3):  # end of session; repeated in case one is dropped
        sock.sendto(session + struct.pack(">QH", seq, 0xFFFF), dest)
        time.sleep(0.05)
    print(f"sent {sent_msgs} messages in {sent_packets} packets to {args.host}:{args.port}", file=sys.stderr)


if __name__ == "__main__":
    main()
