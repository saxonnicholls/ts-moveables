#!/usr/bin/env bash
#
#  run_h2spec.sh - grade http2.hpp against h2spec
#
#  h2spec is the external, unarguable RFC 9113 + RFC 7541 conformance suite:
#  about 150 cases attacking framing, the stream state machine, flow control,
#  HPACK and HTTP semantics. It drives a client at a real server; the server
#  here is ours (tests/h2spec/h2_server.cpp), so what gets graded is the
#  protocol delegate.
#
#  The suite itself is NOT a dependency of this library - it runs from the
#  official Docker image, exactly like Autobahn does for WebSocket.
#
#      make h2spec                    # everything
#      make h2spec SECTIONS='6.5 6.9' # a subset, while iterating
#
#  Exit status is 0 when every case passes, 1 when the grader found failures,
#  and 2 when the grader could not run at all. That distinction is the whole
#  point: "no result" and "bad result" send you looking in different places.
#

set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
outdir="$root/build/h2spec"
port="${PORT:-8888}"
sections="${SECTIONS:-}"
server_bin="$root/build/h2_server"

[ -x "$server_bin" ] || { echo "error: build $server_bin first (make build/h2_server)" >&2; exit 2; }

mkdir -p "$outdir"

# ---------------------------------------------------------------- the runner
# A native h2spec if one is on PATH, otherwise the official Docker image.
h2spec="${H2SPEC:-$(command -v h2spec 2>/dev/null || true)}"
mode="native"
if [ -z "$h2spec" ] || [ ! -x "$h2spec" ]; then
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        mode="docker"
    else
        echo "error: need either h2spec on PATH (set H2SPEC=...) or a running Docker" >&2
        exit 2
    fi
fi
echo "runner: $mode"

# Inside a container the host is not 127.0.0.1. Linux gets --network host so
# loopback is the host's; macOS and Windows reach it via host.docker.internal.
target_host="127.0.0.1"
bind_host="127.0.0.1"
if [ "$mode" = "docker" ]; then
    bind_host="0.0.0.0"
    [ "$(uname -s)" != "Linux" ] && target_host="host.docker.internal"
fi

# ------------------------------------------------------------- run the pair
BIND_HOST="$bind_host" "$server_bin" "$port" > "$outdir/server.log" 2>&1 &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null; wait "$server_pid" 2>/dev/null; }
trap cleanup EXIT

for _ in $(seq 1 50); do
    grep -q "listening" "$outdir/server.log" 2>/dev/null && break
    sleep 0.2
done
grep -q "listening" "$outdir/server.log" || {
    echo "error: h2 server did not start" >&2
    cat "$outdir/server.log" >&2
    exit 2
}

# An empty SECTIONS must expand to no arguments at all, not to one empty one -
# `set -u` plus an empty array is a bash version minefield, so build the whole
# argument list as a single array instead
args=(-h "$target_host" -p "$port" --strict)
if [ -n "$sections" ]; then
    # shellcheck disable=SC2206
    args+=($sections)
fi

echo "running h2spec${sections:+ sections: $sections}"
if [ "$mode" = "native" ]; then
    "$h2spec" "${args[@]}" >"$outdir/h2spec.log" 2>&1
    rc=$?
elif [ "$(uname -s)" = "Linux" ]; then
    docker run --rm --network host summerwind/h2spec "${args[@]}" \
        >"$outdir/h2spec.log" 2>&1
    rc=$?
else
    docker run --rm summerwind/h2spec "${args[@]}" \
        >"$outdir/h2spec.log" 2>&1
    rc=$?
fi

# Distinguish "the suite could not run" from "the suite found failures". The
# image is published for amd64 only, so on an arm64 host without emulation the
# run dies with a manifest error - which is a missing grader, not a
# conformance result, and reporting it as a failure sends you to the wrong file.
if [ "$mode" = "docker" ] && [ "$rc" -ne 0 ]; then
    if grep -qiE "no matching manifest|not match the detected host|exec format error|Cannot connect to the Docker daemon" \
            "$outdir/h2spec.log" 2>/dev/null; then
        echo "error: summerwind/h2spec has no runnable image for $(uname -m)." >&2
        echo "       Run the conformance suite on x86-64, or install h2spec natively" >&2
        echo "       and point H2SPEC at it. This is a missing grader, not a failure." >&2
        exit 2
    fi
fi

cleanup
trap - EXIT

# -------------------------------------------------------------- the verdict
if ! grep -qE "^[0-9]+ tests?, " "$outdir/h2spec.log"; then
    echo "error: h2spec produced no summary - see $outdir/h2spec.log" >&2
    tail -30 "$outdir/h2spec.log" >&2
    exit 2
fi

summary="$(grep -E "^[0-9]+ tests?, " "$outdir/h2spec.log" | tail -1)"
failed="$(echo "$summary" | sed -nE 's/.*, ([0-9]+) failed.*/\1/p')"

echo
echo "h2spec - ts-moveables http2.hpp"
echo "----------------------------------------------------"
echo "  $summary"

if [ "${failed:-0}" -ne 0 ]; then
    echo
    echo "failing cases:"
    grep -E "^\s+×" "$outdir/h2spec.log" | head -60
    echo
    echo "full report: $outdir/h2spec.log"
    exit 1
fi

echo
echo "all cases clean - report: $outdir/h2spec.log"
