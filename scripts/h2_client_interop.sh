#!/usr/bin/env bash
#
#  h2_client_interop.sh - grade http::client's HTTP/2 against servers that are not ours
#
#  The h2 server is graded by h2spec; the client has no such suite, so it is
#  graded by talking to independent implementations instead:
#
#    - Go's standard library server (tests/client/h2_interop/server.go): h2c by
#      prior knowledge and h2 over TLS by ALPN, trailers, a handler that
#      aborts its stream, bodies past every window. Needs Go 1.24 or later.
#    - nghttp2's nghttpd, when it is on PATH: concurrent downloads and a 404.
#
#  Neither is a dependency of the library; like h2spec and Autobahn they are
#  graders. Exit status: 0 every check passed, 1 a check failed, 2 the grade
#  could not run (no Go, a server that would not start).
#

set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$root/build/h2_interop"
client_bin="$root/build/h2_client_interop"
[ -x "$client_bin" ] || { echo "error: build $client_bin first (make build/h2_client_interop)" >&2; exit 2; }
command -v go >/dev/null || { echo "error: Go is not on PATH; the Go grader cannot run" >&2; exit 2; }
mkdir -p "$work"

pids=()
cleanup() { for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

go build -o "$work/go_server" "$root/tests/client/h2_interop/server.go" || { echo "error: go build failed" >&2; exit 2; }
"$work/go_server" -ca "$work/go_ca.pem" > "$work/go_ports" 2> "$work/go_server.log" &
pids+=($!)
for _ in $(seq 1 100); do
    grep -q "h2c=" "$work/go_ports" 2>/dev/null && break
    sleep 0.1
done
read -r h2c_field tls_field < "$work/go_ports" || { echo "error: the Go server did not start" >&2; exit 2; }
args=(--go-h2c "${h2c_field#h2c=}" --go-tls "${tls_field#tls=}" --ca "$work/go_ca.pem")

if command -v nghttpd >/dev/null; then
    docs="$work/nghttpd_docs"
    mkdir -p "$docs"
    head -c 3145728 /dev/zero | tr '\0' 'n' > "$docs/three_mib.bin"
    port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
    nghttpd --no-tls -a 127.0.0.1 -d "$docs" "$port" > "$work/nghttpd.log" 2>&1 &
    pids+=($!)
    for _ in $(seq 1 50); do
        (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break
        sleep 0.1
    done
    args+=(--nghttpd "$port" --nghttpd-file three_mib.bin --nghttpd-size 3145728)
else
    echo "note: nghttpd not on PATH - graded against Go only"
fi

timeout 120 "$client_bin" "${args[@]}"
status=$?
[ $status -eq 0 ] && exit 0
echo "h2 client interop FAILED (status $status); server logs in $work" >&2
exit 1
