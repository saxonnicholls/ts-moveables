#!/usr/bin/env python3
#
#  best_of_passes.py
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  The best of several run_comparison.sh passes, cell by cell: for each vocabulary, library and thread count, the
#  fastest of the passes, in MB/s, with the most documents any pass found wrong. On a shared machine the slow passes
#  are the ones something else interfered with, so the best is the closest to what each encoder can do; every pass
#  ran every library with the same repetitions.
#
#      python3 scripts/tokenizer/best_of_passes.py pass1.tsv pass2.tsv ... [--markdown]
#

import collections
import sys

THREADS = [1, 2, 4, 8, 16, 32]


def main():
    files = [a for a in sys.argv[1:] if not a.startswith("--")]
    markdown = "--markdown" in sys.argv
    best = collections.defaultdict(float)
    wrong = collections.defaultdict(int)
    for f in files:
        for line in open(f):
            p = line.rstrip("\n").split("\t")
            if len(p) < 8 or p[0] != "RESULT":
                continue
            vocab, lib, t, sec, size, differ = p[1], p[2], int(p[3]), float(p[4]), float(p[5]), int(p[7])
            # One row per library: its one-thread call, its cold first pass, and its batch at each thread count
            if lib.endswith(" cold"):
                key, col = lib[: -len(" cold")], "cold"
            elif lib.endswith(" batch"):
                key, col = lib[: -len(" batch")], t
            else:
                key, col = lib, "single"
            best[(vocab, key, col)] = max(best[(vocab, key, col)], size / sec / 1e6)
            wrong[(vocab, key)] = max(wrong[(vocab, key)], differ)
    cols = ["single", "cold"] + THREADS
    heads = ["1 thread", "cold"] + ["batch %d" % t for t in THREADS]
    for vocab in ["cl100k", "rust32k"]:
        libs = {k[1] for k in best if k[0] == vocab}
        order = sorted(libs, key=lambda l: -max(best.get((vocab, l, c), 0) for c in cols))
        print("\n%s: best of %d passes, MB/s" % (vocab, len(files)))
        if markdown:
            print("\n| library | " + " | ".join(heads) + " | wrong docs |")
            print("|---|" + "---:|" * len(heads) + "---:|")
        for l in order:
            cells = []
            for c in cols:
                v = best.get((vocab, l, c))
                cells.append("—" if v is None else ("%.0f" % v if v >= 20 else "%.1f" % v))
            if markdown:
                print("| %s | %s | %d |" % (l, " | ".join(cells), wrong[(vocab, l)]))
            else:
                print("%-30s %s  %d wrong" % (l, " ".join("%7s" % c for c in cells), wrong[(vocab, l)]))


if __name__ == "__main__":
    main()
