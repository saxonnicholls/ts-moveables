#!/usr/bin/env bash
#
#  run_autobahn.sh - grade websocket.hpp against Autobahn|Testsuite
#
#  Autobahn is the external, unarguable RFC 6455 conformance suite: several
#  hundred cases attacking framing, fragmentation, UTF-8, control frames,
#  close codes and limits. It drives a fuzzing client at an echo server; the
#  echo server here is ours (tests/autobahn/ws_echo_server.cpp), so what gets
#  graded is the protocol delegate.
#
#  The suite itself is NOT a dependency of this library - it is fetched into a
#  throwaway virtualenv (or run from Docker), exactly like the moodycamel and
#  cpp-httplib comparison headers.
#
#      make autobahn                  # everything
#      make autobahn CASES='1.*,7.*'  # a subset, while iterating
#
#  Results land in build/autobahn/index.json and are summarised at the end.
#  Exit status is non-zero if any case is outright FAILED.
#

set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
outdir="$root/build/autobahn"
port="${PORT:-9001}"
cases="${CASES:-*}"
server_bin="$root/build/ws_echo_server"

[ -x "$server_bin" ] || { echo "error: build $server_bin first (make build/ws_echo_server)" >&2; exit 1; }

mkdir -p "$outdir"

# ---------------------------------------------------------------- the runner
# A native wstest if one is on PATH, otherwise the official Docker image.
# Note: the PyPI `autobahntestsuite` package is a Python 2 codebase and does
# not import under Python 3, so Docker is the practical route in 2026.
wstest="${WSTEST:-$(command -v wstest 2>/dev/null || true)}"
mode="native"
if [ -z "$wstest" ] || [ ! -x "$wstest" ]; then
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        mode="docker"
    else
        echo "error: need either wstest on PATH (set WSTEST=...) or a running Docker" >&2
        exit 1
    fi
fi
echo "runner: $mode"

# ------------------------------------------------------------------ the spec
IFS=',' read -ra case_list <<< "$cases"
case_json=""
for c in "${case_list[@]}"; do
    [ -n "$case_json" ] && case_json="$case_json, "
    case_json="$case_json\"$c\""
done

# Inside a container the host is not 127.0.0.1. Linux gets --network host so
# loopback is the host's; macOS and Windows reach it via host.docker.internal.
target_host="127.0.0.1"
if [ "$mode" = "docker" ] && [ "$(uname -s)" != "Linux" ]; then
    target_host="host.docker.internal"
fi

spec_outdir="$outdir"
[ "$mode" = "docker" ] && spec_outdir="/reports"

cat > "$outdir/fuzzingclient.json" <<EOF
{
   "outdir": "$spec_outdir",
   "servers": [{ "agent": "ts-moveables", "url": "ws://$target_host:$port" }],
   "cases": [$case_json],
   "exclude-cases": [],
   "exclude-agent-cases": {}
}
EOF

# ------------------------------------------------------------- run the pair
BIND_HOST="0.0.0.0" "$server_bin" "$port" > "$outdir/server.log" 2>&1 &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null; wait "$server_pid" 2>/dev/null; }
trap cleanup EXIT

for _ in $(seq 1 50); do
    grep -q "listening" "$outdir/server.log" 2>/dev/null && break
    sleep 0.2
done
grep -q "listening" "$outdir/server.log" || { echo "error: echo server did not start"; cat "$outdir/server.log"; exit 1; }

echo "running Autobahn cases: $cases"
if [ "$mode" = "native" ]; then
    "$wstest" --mode fuzzingclient --spec "$outdir/fuzzingclient.json" \
        >"$outdir/wstest.log" 2>&1
elif [ "$(uname -s)" = "Linux" ]; then
    docker run --rm --network host \
        -v "$outdir:/config" -v "$outdir:/reports" \
        crossbario/autobahn-testsuite \
        wstest --mode fuzzingclient --spec /config/fuzzingclient.json \
        >"$outdir/wstest.log" 2>&1
    docker_rc=$?
else
    docker run --rm \
        -v "$outdir:/config" -v "$outdir:/reports" \
        crossbario/autobahn-testsuite \
        wstest --mode fuzzingclient --spec /config/fuzzingclient.json \
        >"$outdir/wstest.log" 2>&1
    docker_rc=$?
fi

# Distinguish "the suite could not run" from "the suite found failures". The
# official image is published for amd64 only, so on an arm64 host the run dies
# with a manifest error - which is a missing grader, not a conformance result,
# and reporting it as a parse failure sends you looking in the wrong place.
if [ "$mode" = "docker" ] && [ "${docker_rc:-0}" -ne 0 ]; then
    if grep -qiE "no matching manifest|not match the detected host|exec format error" \
            "$outdir/wstest.log" 2>/dev/null; then
        echo "error: crossbario/autobahn-testsuite has no image for $(uname -m)." >&2
        echo "       Run the conformance suite on x86-64, or install wstest natively" >&2
        echo "       and point WSTEST at it. This is a missing grader, not a failure." >&2
        exit 2
    fi
    echo "error: the Autobahn container exited $docker_rc - see $outdir/wstest.log" >&2
    tail -20 "$outdir/wstest.log" >&2
    exit 2
fi
echo "wstest finished"

cleanup
trap - EXIT

# -------------------------------------------------------------- the verdict
python3 - "$outdir" <<'PY'
import json, os, sys, collections
outdir = sys.argv[1]
path = os.path.join(outdir, "index.json")
if not os.path.exists(path):
    print("error: no index.json - see", os.path.join(outdir, "wstest.log"))
    sys.exit(1)

with open(path) as f:
    idx = json.load(f)

results = next(iter(idx.values()))
tally = collections.Counter()
failed = []
for case, info in sorted(results.items()):
    behavior = info.get("behavior", "?")
    close = info.get("behaviorClose", "?")
    # A case is only a pass when both the exchange and the close were right
    ok = behavior in ("OK", "NON-STRICT", "INFORMATIONAL", "UNIMPLEMENTED")
    ok_close = close in ("OK", "INFORMATIONAL")
    tally[behavior] += 1
    if not (ok and ok_close):
        failed.append((case, behavior, close))

total = sum(tally.values())
print()
print("Autobahn|Testsuite - ts-moveables websocket.hpp")
print("-" * 52)
for k in sorted(tally):
    print(f"  {k:<16} {tally[k]:>4}")
print(f"  {'TOTAL':<16} {total:>4}")

if failed:
    print(f"\n{len(failed)} case(s) not clean:")
    for case, b, c in failed[:40]:
        print(f"  {case:<10} behavior={b} close={c}")
    if len(failed) > 40:
        print(f"  ... and {len(failed) - 40} more")
    print(f"\nfull report: {os.path.join(outdir, 'index.html')}")
    sys.exit(1)

print(f"\nall {total} cases clean - report: {os.path.join(outdir, 'index.html')}")
PY
