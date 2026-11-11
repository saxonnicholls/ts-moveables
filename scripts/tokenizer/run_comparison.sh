#!/bin/sh
#
#  run_comparison.sh
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  One timing pass of every encoder on prepare.py's data, same reps for all, written as RESULT lines
#  (vocab, library, threads, seconds, bytes, tokens, documents that differ from tiktoken) to <out.tsv>:
#    - this demo (build/tokenizer_bench): "encode, 1 thread" (best of reps, warm), its cold first pass, and batch with N-1 workers + the caller = N
#    - rust_baselines, once per thread count with RAYON_NUM_THREADS=N (one-thread rows on the first)
#    - cpp_baselines (quicktok, TokenDagger), every thread count
#    - compare_python_libs.py (tiktoken, TokenDagger from Python), every thread count
#  OURS=<path> times another build of tokenizer_bench (default: the repo's build/tokenizer_bench).
#  Then summarize_comparison.py turns one or more of these files into the tables.
#
#      scripts/tokenizer/run_comparison.sh <data dir> <tokenizer.json> <cpp_baselines binary> \
#          <quicktok data dir> <python> <out.tsv> [reps]
#
set -eu
data=$1; json=$2; cpp=$3; qtdata=$4; py=$5; out=$6; reps=${7:-5}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
ours=${OURS:-$repo/build/tokenizer_bench}
threads="1 2 4 8 16 32"
: > "$out"
log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

for v in cl100k rust32k; do
    log "this demo, $v"
    "$ours" "$data" "$v" --threads 1,3,7,15,31 --reps "$reps" | awk -v v="$v" '
        /^check/ && !/ 0 of | 0 documents|same as serial/ { bad = 1 }
        /FAILED/ { bad = 1 }
        /^[a-z0-9]+: [0-9]+ documents/ { bytes = $4 * 1e6; tokens = $8 }
        /^  encode, 1 thread +[0-9]/ { printf "RESULT\t%s\tthis demo\t1\t%.6f\t%s\t%s\t%d\n", v, $4 / 1e3, bytes, tokens, bad }
        /^  encode, 1 thread, cold cache/ { printf "RESULT\t%s\tthis demo cold\t1\t%.6f\t%s\t%s\t%d\n", v, $6 / 1e3, bytes, tokens, bad }
        /^  batch, [0-9]+ workers \+ caller/ {
            k = $2 + 1; if (k <= 32) printf "RESULT\t%s\tthis demo batch\t%d\t%.6f\t%s\t%s\t%d\n", v, k, $6 / 1e3, bytes, tokens, bad }
    ' >> "$out"
done

for t in $threads; do
    log "rust_baselines, RAYON_NUM_THREADS=$t"
    single=""; [ "$t" = 1 ] && single="--single"
    RAYON_NUM_THREADS=$t "$here/rust_baselines/target/release/rust_baselines" "$data" --json "$json" --reps "$reps" $single \
        | grep '^RESULT' >> "$out"
done

log "cpp_baselines"
"$cpp" "$data" "$qtdata" --threads 1,2,4,8,16,32 --reps "$reps" | grep '^RESULT' >> "$out"

log "python"
"$py" -I "$here/compare_python_libs.py" "$data" --threads 1,2,4,8,16,32 --reps "$reps" | grep '^RESULT' >> "$out"
log "done: $out"
