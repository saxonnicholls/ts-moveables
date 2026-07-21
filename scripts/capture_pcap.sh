#!/usr/bin/env bash
#
#  capture_pcap.sh - capture live packets for pcap_replay_demo
#
#  tcpdump's -w writes exactly the format the demo reads: classic pcap.
#  (Wireshark saves pcapng by default - convert with: tcpdump -r in.pcapng -w out.pcap)
#
#  usage:    scripts/capture_pcap.sh [interface] [packet_count] [outfile]
#  examples: scripts/capture_pcap.sh                      # default interface, 100k packets
#            scripts/capture_pcap.sh en0 1000000 big.pcap
#
#  Raw capture needs privileges, hence sudo. Stop early with ctrl-c - whatever
#  was captured so far is still valid and replayable.

set -euo pipefail

IFACE="${1:-}"
COUNT="${2:-100000}"
OUT="${3:-capture.pcap}"

command -v tcpdump >/dev/null 2>&1 || { echo "error: tcpdump not found" >&2; exit 1; }

# Auto-detect the default-route interface: en0-style on macOS, eth0/enp-style on Linux
if [ -z "$IFACE" ]; then
    case "$(uname -s)" in
        Darwin) IFACE=$(route -n get default 2>/dev/null | awk '/interface:/{print $2}') ;;
        Linux)  IFACE=$(ip route show default 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i=="dev"){print $(i+1); exit}}') ;;
    esac
    if [ -z "${IFACE:-}" ]; then
        echo "error: could not detect the default interface - pass one explicitly" >&2
        echo "       (list them with: tcpdump -D)" >&2
        exit 1
    fi
fi

echo "capturing $COUNT packets on $IFACE -> $OUT   (ctrl-c to stop early)"

# -s 0: full packets, so the demo's payload hashing covers every byte
sudo tcpdump -i "$IFACE" -c "$COUNT" -s 0 -w "$OUT" || true
sudo chown "$(id -un)" "$OUT"

[ -s "$OUT" ] || { echo "error: nothing captured" >&2; exit 1; }

echo
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)) - now replay it:"
echo "  make build/pcap_replay_demo && ./build/pcap_replay_demo $OUT"
