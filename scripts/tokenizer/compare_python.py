#!/usr/bin/env python3
#
#  compare_python.py
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  The Python-side baselines on prepare.py's data: tiktoken one thread and encode_ordinary_batch, rustbpe
#  encode and batch_encode. These include building Python lists; rust_baselines/ is the native comparison.
#
#      python scripts/tokenizer/compare_python.py build/tokdata
#
import base64, os, pathlib, struct, sys, time
import rustbpe, tiktoken

d = pathlib.Path(sys.argv[1])
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

def best(fn, reps=5):
    b = 1e9; r = None
    for _ in range(reps):
        t = time.perf_counter(); r = fn(); b = min(b, time.perf_counter() - t)
    return r, b

def row(what, toks, dt):
    n = sum(map(len, toks))
    print(f"  {what:44s} {dt*1e3:9.1f} ms {n/dt/1e6:8.2f} M tok/s {total/dt/1e6:8.1f} MB/s")

all_text = "".join(docs)
for name in ["cl100k", "rust32k"]:
    pat = (d / f"{name}.pattern").read_text()
    enc = tiktoken.Encoding(name=name, pat_str=pat, mergeable_ranks=ranks(name), special_tokens={})
    print(name)
    toks, dt = best(lambda: [enc.encode_ordinary(x) for x in docs]); row("tiktoken, 1 thread", toks, dt)
    for t in [8, 16, 32]:
        toks, dt = best(lambda: enc.encode_ordinary_batch(docs, num_threads=t)); row(f"tiktoken encode_ordinary_batch, {t} threads", toks, dt)
    toks, dt = best(lambda: [enc.encode_ordinary(all_text)], reps=3); row("tiktoken, one text", toks, dt)
    if name == "rust32k":
        tok = rustbpe.Tokenizer(); tok.train_from_iterator(iter(docs), 32768, pattern=pat)
        toks, dt = best(lambda: [tok.encode(x) for x in docs], reps=3); row("rustbpe encode, 1 thread", toks, dt)
        toks, dt = best(lambda: tok.batch_encode(docs)); row(f"rustbpe batch_encode (rayon, {os.cpu_count()} threads)", toks, dt)
        toks, dt = best(lambda: [tok.encode(all_text)], reps=3); row("rustbpe, one text", toks, dt)
