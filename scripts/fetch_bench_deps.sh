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

# Pinned to release tags, not master.
#
# A head-to-head number is a claim about a specific version of the other side.
# Fetching master means the comparison in the README was measured against
# whatever happened to be HEAD that morning, nobody can reproduce it, and a
# change upstream silently moves our published figures. Override deliberately
# to try a newer one: HTTPLIB_TAG=v0.52.0 ./scripts/fetch_bench_deps.sh
CONCURRENTQUEUE_TAG="${CONCURRENTQUEUE_TAG:-v1.0.5}"
READERWRITERQUEUE_TAG="${READERWRITERQUEUE_TAG:-v1.0.7}"
HTTPLIB_TAG="${HTTPLIB_TAG:-v0.51.0}"

cq=https://raw.githubusercontent.com/cameron314/concurrentqueue/$CONCURRENTQUEUE_TAG
rw=https://raw.githubusercontent.com/cameron314/readerwriterqueue/$READERWRITERQUEUE_TAG

fetch() {  # url dest
    if [ -f "$2" ]; then
        echo "have $(basename "$2")"
        return
    fi
    echo "fetching $(basename "$2")"
    curl -sSL -o "$2" "$1"
}

command -v curl >/dev/null 2>&1 || { echo "error: curl not found" >&2; exit 1; }

# fetch() keeps whatever is already on disk, so without this a checkout that
# pulled these from master before they were pinned would quietly keep using it -
# a pin that does not dislodge the unpinned copy is not a pin. Stamp the tags
# and re-fetch when they change.
stamp="$dir/.versions"
want="cq=$CONCURRENTQUEUE_TAG rw=$READERWRITERQUEUE_TAG hl=$HTTPLIB_TAG"
if [ ! -f "$stamp" ] || [ "$(cat "$stamp" 2>/dev/null)" != "$want" ]; then
    [ -f "$stamp" ] && echo "pinned versions changed - refetching"
    rm -f "$dir/concurrentqueue.h" "$dir/readerwriterqueue.h" \
          "$dir/atomicops.h" "$dir/httplib.h"
fi

hl=https://raw.githubusercontent.com/yhirose/cpp-httplib/$HTTPLIB_TAG

fetch "$cq/concurrentqueue.h"  "$dir/concurrentqueue.h"
fetch "$rw/readerwriterqueue.h" "$dir/readerwriterqueue.h"
fetch "$rw/atomicops.h"         "$dir/atomicops.h"
fetch "$hl/httplib.h"           "$dir/httplib.h"

echo "$want" > "$stamp"

echo "comparison headers ready in $dir"
echo "  cpp-httplib       $HTTPLIB_TAG"
echo "  concurrentqueue   $CONCURRENTQUEUE_TAG"
echo "  readerwriterqueue $READERWRITERQUEUE_TAG"
