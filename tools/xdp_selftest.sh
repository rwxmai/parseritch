#!/usr/bin/env bash
# End-to-end tests of the AF_XDP receive path on a virtual Ethernet pair.
#
#   sudo tools/xdp_selftest.sh BUILD_DIR      (a build configured with -DITCH_ENABLE_XDP=ON)
#
# Creates a network namespace holding one end of a veth pair and attaches the
# XDP filter to the other end (generic mode). From inside the namespace it
# replays a synthetic ITCH session as a MoldUDP64 feed, then checks that
# feed_handler --xdp received every message with no gaps and no book errors.
#
#   case 1, unicast:   feed to 10.77.0.1:PORT, filter on port only; ping to
#                      the host must keep working (non-matching traffic
#                      stays on the kernel stack)
#   case 2, multicast: feed to 239.77.0.1:PORT, filter on group + port, while
#                      decoy datagrams go to 10.77.0.1:PORT; the decoys must
#                      not reach the socket
#
# Needs root (netns, veth, BPF) and a kernel with CONFIG_XDP_SOCKETS.
set -euo pipefail

B=$(cd "${1:-build}" && pwd)
SRC=$(cd "$(dirname "$0")/.." && pwd)
NS=itchxdp$$
HOST_IF=$(echo "ixv0$$" | cut -c1-15)
PEER_IF=$(echo "ixv1$$" | cut -c1-15)
PORT=26477
GROUP=239.77.0.1
WORK=$(mktemp -d)
RX_PID=

cleanup() {
    [[ -n $RX_PID ]] && kill "$RX_PID" 2>/dev/null || true
    ip netns del "$NS" 2>/dev/null || true
    ip link del "$HOST_IF" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

ip netns add "$NS"
ip link add "$HOST_IF" type veth peer name "$PEER_IF"
ip link set "$PEER_IF" netns "$NS"
ip addr add 10.77.0.1/24 dev "$HOST_IF"
ip link set "$HOST_IF" up
ip -n "$NS" addr add 10.77.0.2/24 dev "$PEER_IF"
ip -n "$NS" link set "$PEER_IF" up
ip -n "$NS" link set lo up
ip -n "$NS" route add 239.0.0.0/8 dev "$PEER_IF"  # multicast out of the veth

expected=$("$B/itch_synth" "$WORK/session.itch" --events 200000 --symbols 64 | sed -n 's/.*: \([0-9]*\) messages.*/\1/p')
packets=0
echo "session: $expected messages"
fail=0

start_rx() {  # feed_handler args...
    "$B/feed_handler" --xdp "$HOST_IF:0" --stats 3600 "$@" >"$WORK/rx.log" 2>&1 &
    RX_PID=$!
    sleep 1
    kill -0 "$RX_PID" 2>/dev/null || { cat "$WORK/rx.log"; echo "FAIL: receiver did not start"; exit 1; }
}

finish_rx() {  # case name
    for _ in $(seq 100); do kill -0 "$RX_PID" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$RX_PID" 2>/dev/null; then
        kill "$RX_PID"; cat "$WORK/rx.log"; echo "FAIL [$1]: receiver did not see end of session"; exit 1
    fi
    wait "$RX_PID" || true
    RX_PID=
    sed -n '/^AF_XDP/p;/^live:/p;/^xdp:/p;/^  messages/p;/book integrity/p' "$WORK/rx.log"
    local got
    got=$(sed -n 's/^  messages *\([0-9]*\).*/\1/p' "$WORK/rx.log")
    [[ "$got" == "$expected" ]] || { echo "FAIL [$1]: received $got of $expected messages"; fail=1; }
    grep -q "gaps=0 lost=0" "$WORK/rx.log" || { echo "FAIL [$1]: sequence gaps"; fail=1; }
    grep -q "unknown_ref=0 missing_level=0 overfill=0" "$WORK/rx.log" || { echo "FAIL [$1]: book integrity"; fail=1; }
    grep -q "not_udp=0" "$WORK/rx.log" || { echo "FAIL [$1]: non-UDP frames reached the socket"; fail=1; }
    grep -q "fill_starve=0" "$WORK/rx.log" || { echo "FAIL [$1]: fill ring starved"; fail=1; }
}

send_feed() {  # destination
    ip netns exec "$NS" python3 "$SRC/tools/mold_send.py" "$WORK/session.itch" "$1" "$PORT" --pps 20000 2>"$WORK/tx.log"
    cat "$WORK/tx.log"
    packets=$(sed -n 's/.* in \([0-9]*\) packets.*/\1/p' "$WORK/tx.log")
}

# ---- case 1: unicast, port filter, pass-through ----------------------------
echo "== case 1: unicast"
start_rx --port "$PORT"
if ip netns exec "$NS" sh -c 'command -v ping' >/dev/null 2>&1; then
    if ip netns exec "$NS" ping -c 3 -W 1 10.77.0.1 >/dev/null; then
        echo "ok: ICMP to the host still reaches the kernel stack with the program attached"
    else
        echo "FAIL [unicast]: ping broken while XDP attached: non-matching traffic is being hijacked"; fail=1
    fi
else
    echo "note: ping not installed; pass-through check skipped"
fi
send_feed 10.77.0.1
finish_rx unicast

# ---- case 2: multicast group filter with unicast decoys ----------------------
echo "== case 2: multicast $GROUP"
start_rx --mcast "$GROUP:$PORT"
ip netns exec "$NS" python3 - "$PORT" <<'EOF' &
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for _ in range(200):  # same port, unicast destination: must stay on the stack
    s.sendto(b"decoy", ("10.77.0.1", int(sys.argv[1])))
    time.sleep(0.005)
EOF
DECOY_PID=$!
send_feed "$GROUP"
wait "$DECOY_PID"
finish_rx multicast
frames=$(sed -n 's/^xdp: frames=\([0-9]*\).*/\1/p' "$WORK/rx.log")
# Every data packet plus 1-3 end-of-session packets (the sender repeats it);
# any leaked decoy would add up to 200 more.
extra=$((frames - packets))
if (( extra >= 1 && extra <= 3 )); then
    echo "ok: only group traffic reached the socket ($frames frames for $packets data packets; 200 decoys filtered)"
else
    echo "FAIL [multicast]: socket saw $frames frames for $packets data packets (decoys leaked?)"; fail=1
fi

[[ $fail == 0 ]] && echo "PASS: AF_XDP unicast and multicast cases"
exit $fail
