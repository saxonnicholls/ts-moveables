#!/usr/bin/env bash
#
#  fetch_bench_deps.sh - fetch the header-only comparison dependencies
#
#  These are NOT part of the library, which stays dependency-free. They are used
#  only by `make bench-compare` for the head-to-head throughput comparison
#  against moodycamel's lock-free queues (Simplified BSD, (c) Cameron Desrochers)
#  and by `make bench-http` for the HTTP head-to-head against cpp-httplib
#  (MIT, (c) Yuji Hirose).
#  Fetched into benchmarks/third_party/ (gitignored), so nothing third-party is
#  committed to this repo.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dir="$root/benchmarks/third_party"
mkdir -p "$dir"

cq=https://raw.githubusercontent.com/cameron314/concurrentqueue/master
rw=https://raw.githubusercontent.com/cameron314/readerwriterqueue/master

fetch() {  # url dest
    if [ -f "$2" ]; then
        echo "have $(basename "$2")"
        return
    fi
    echo "fetching $(basename "$2")"
    curl -sSL -o "$2" "$1"
}

command -v curl >/dev/null 2>&1 || { echo "error: curl not found" >&2; exit 1; }

hl=https://raw.githubusercontent.com/yhirose/cpp-httplib/master

fetch "$cq/concurrentqueue.h"  "$dir/concurrentqueue.h"
fetch "$rw/readerwriterqueue.h" "$dir/readerwriterqueue.h"
fetch "$rw/atomicops.h"         "$dir/atomicops.h"
fetch "$hl/httplib.h"           "$dir/httplib.h"

echo "comparison headers ready in $dir"
