#!/usr/bin/env python3
#
#  compare_python_libs.py
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  The Python-side rows for run_comparison.sh, on prepare.py's data: tiktoken (encode_ordinary in a loop,
#  encode_ordinary_batch) and, if it is installed, TokenDagger (encode_ordinary in a loop, encode_batch,
#  which is a ThreadPoolExecutor over its C++ encode with the GIL released). Every result is checked
#  against tiktoken's tokens. These include building Python lists; cpp_baselines/ times TokenDagger's C++
#  core without Python.
#
#      python compare_python_libs.py <data dir> [--threads 1,2,4,8,16,32] [--reps 5]
#
import base64, pathlib, struct, sys, time

d = pathlib.Path(sys.argv[1])
threads = [1, 2, 4, 8, 16, 32]
reps = 5
args = sys.argv[2:]
for i, a in enumerate(args):
    if a == "--threads":
        threads = [int(x) for x in args[i + 1].split(",")]
    if a == "--reps":
        reps = int(args[i + 1])

raw = (d / "docs.bin").read_bytes()
n = struct.unpack_from("<I", raw, 0)[0]; at = 4; docs = []
for _ in range(n):
    l = struct.unpack_from("<I", raw, at)[0]; at += 4; docs.append(raw[at:at + l].decode()); at += l
total = sum(len(x.encode()) for x in docs)

def ranks(name):
    out = {}
    for line in (d / f"{name}.ranks").read_text().splitlines():
        b, r = line.split(); out[base64.b64decode(b)] = int(r)
    return out

def expected(name):
    tb = (d / f"{name}.tokens").read_bytes()
    m = struct.unpack_from("<I", tb, 0)[0]; at = 4; out = []
    for _ in range(m):
        l = struct.unpack_from("<I", tb, at)[0]; at += 4
        out.append(list(struct.unpack_from(f"<{l}I", tb, at))); at += 4 * l
    return out

def best(fn):
    b = 1e9; r = None
    for _ in range(reps):
        t = time.perf_counter(); r = fn(); b = min(b, time.perf_counter() - t)
    return r, b

libs = []
import tiktoken
libs.append(("tiktoken (Python) " + tiktoken.__version__, tiktoken,
             lambda enc, x: enc.encode_ordinary(x),
             lambda enc, t: enc.encode_ordinary_batch(docs, num_threads=t)))
try:
    import tokendagger
    # encode with no specials is the call its encode_batch makes, and faster than its encode_ordinary
    libs.append(("TokenDagger (Python) " + tokendagger.__version__, tokendagger,
                 lambda enc, x: enc.encode(x, disallowed_special=()),
                 lambda enc, t: enc.encode_batch(docs, num_threads=t, disallowed_special=())))
except ImportError:
    print("TokenDagger is not installed; skipped")

for name in ["cl100k", "rust32k"]:
    pat = (d / f"{name}.pattern").read_text().rstrip("\r\n")
    exp = expected(name); ntok = sum(map(len, exp)); rk = ranks(name)
    print(f"{name}: {len(docs)} documents, {total/1e6:.1f} MB, {ntok} tokens")
    for lib, mod, one, batch in libs:
        enc = mod.Encoding(name=name, pat_str=pat, mergeable_ranks=rk, special_tokens={})
        def row(what, t, toks, dt):
            bad = sum(1 for a, b in zip(toks, exp) if list(a) != b) + abs(len(toks) - len(exp))
            print(f"  {what:34s} {t:3d} threads {dt*1e3:9.1f} ms {ntok/dt/1e6:8.2f} M tok/s {total/dt/1e6:8.1f} MB/s  {bad} docs differ")
            print(f"RESULT\t{name}\t{what}\t{t}\t{dt:.6f}\t{total}\t{ntok}\t{bad}", flush=True)
        toks, dt = best(lambda: [one(enc, x) for x in docs]); row(lib, 1, toks, dt)
        for t in threads:
            toks, dt = best(lambda: batch(enc, t)); row(lib + " batch", t, toks, dt)
