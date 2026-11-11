#!/usr/bin/env python3
#
#  summarize_comparison.py
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  Turns run_comparison.sh's RESULT files into markdown: per vocabulary, one row per library, MB/s and
#  M tokens/s at one thread and in batch at each thread count, and whether every document's tokens equal
#  tiktoken's. Given several files (passes), it prints each pass, then the spread between them per cell.
#
#      python summarize_comparison.py pass1.tsv [pass2.tsv ...]
#
import collections, sys

THREADS = [1, 2, 4, 8, 16, 32]

def load(path):
    rows = {}
    bytes_ = {}
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if len(f) != 8 or f[0] != "RESULT":
            continue
        _, vocab, lib, t, s, b, tok, bad = f
        rows[(vocab, lib, int(t))] = (float(s), int(tok), int(bad))
        if lib != "this demo" and not lib.startswith("this demo "):
            bytes_[vocab] = int(float(b))
    return rows, bytes_

def base(lib):
    return lib[:-6] if lib.endswith(" batch") else lib

def table(rows, bytes_, vocab, unit):
    libs = collections.OrderedDict()
    for (v, lib, t) in rows:
        if v == vocab:
            libs.setdefault(base(lib), None)
    head = "| library | correct | 1 thread | " + " | ".join(f"batch {t}" for t in THREADS) + " |"
    out = [head, "|---|---|" + "---:|" * (len(THREADS) + 1)]
    def cell(key):
        if key not in rows:
            return "n/a"
        s, tok, bad = rows[key]
        x = bytes_[vocab] / s / 1e6 if unit == "MB/s" else tok / s / 1e6
        return (f"{x:,.1f}" if unit == "MB/s" else f"{x:,.2f}") + ("" if bad == 0 else f" ✗{bad}")
    for lib in libs:
        keys = [k for k in rows if k[0] == vocab and base(k[1]) == lib]
        bad = sum(rows[k][2] for k in keys)
        correct = "yes" if bad == 0 else "no"
        cells = [cell((vocab, lib, 1))] + [cell((vocab, lib + " batch", t)) for t in THREADS]
        out.append(f"| {lib} | {correct} | " + " | ".join(cells) + " |")
    return "\n".join(out)

passes = [load(p) for p in sys.argv[1:]]
for i, (rows, bytes_) in enumerate(passes, 1):
    for vocab in ["cl100k", "rust32k"]:
        for unit in ["MB/s", "M tokens/s"]:
            print(f"\n### pass {i}: {vocab}, {unit} (✗n: n documents differ from tiktoken)\n")
            print(table(rows, bytes_, vocab, unit))

if len(passes) > 1:
    print("\n### spread between passes (max/min - 1, cells over 5%)\n")
    keys = set.intersection(*(set(r) for r, _ in passes))
    for k in sorted(keys):
        ts = [r[k][0] for r, _ in passes]
        sp = max(ts) / min(ts) - 1
        if sp > 0.05:
            print(f"- {k[0]} {k[1]} @{k[2]}: " + ", ".join(f"{bytes_[k[0]]/t/1e6:,.1f}" for t, (_, bytes_) in zip(ts, passes)) + f" MB/s ({sp*100:.0f}%)")
