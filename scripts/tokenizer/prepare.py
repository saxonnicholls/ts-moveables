#!/usr/bin/env python3
#
#  prepare.py
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  Writes the data demos/tokenizer/tokenizer_bench reads: docs.bin (the Python standard library's sources,
#  one document per file), <vocab>.ranks (tiktoken's base64 format), <vocab>.pattern (each vocabulary's own
#  split pattern) and <vocab>.tokens (tiktoken's tokens for every document), for cl100k_base and for a
#  32,768-token vocabulary rustbpe trains on the corpus. Needs tiktoken and rustbpe (pip install tiktoken
#  rustbpe); cl100k_base is downloaded by tiktoken on first use.
#
#      python scripts/tokenizer/prepare.py build/tokdata
#
# Prepares the tokenizer comparison: a corpus, two vocabularies, the expected tokens, and the single-thread
# timings of tiktoken and rustbpe. Writes into the directory given (default: ./data).
import base64, pathlib, struct, sys, time, sysconfig
import rustbpe, tiktoken

GPT4 = r"""'(?i:[sdmt]|ll|ve|re)|[^\r\n\p{L}\p{N}]?+\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]++[\r\n]*|\s*[\r\n]|\s+(?!\S)|\s+"""
out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "data"); out.mkdir(exist_ok=True)

# Corpus: the Python standard library's sources (code, comments, docstrings) and its English documentation,
# one document per file, up to 40 MB, in a fixed order
lib = pathlib.Path(sysconfig.get_paths()["stdlib"])
files = sorted(p for p in lib.rglob("*.py") if "site-packages" not in p.parts)
docs, total = [], 0
for p in files:
    try:
        t = p.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        continue
    docs.append(t); total += len(t.encode())
    if total > 40_000_000:
        break
print(f"corpus: {len(docs)} documents, {total/1e6:.1f} MB")

def write_docs(path, docs):
    with open(path, "wb") as f:
        f.write(struct.pack("<I", len(docs)))
        for d in docs:
            b = d.encode(); f.write(struct.pack("<I", len(b))); f.write(b)

def write_ranks(path, ranks):
    with open(path, "w") as f:
        for k, v in sorted(ranks.items(), key=lambda kv: kv[1]):
            f.write(f"{base64.b64encode(k).decode()} {v}\n")

def write_tokens(path, all_tokens):
    with open(path, "wb") as f:
        f.write(struct.pack("<I", len(all_tokens)))
        for t in all_tokens:
            f.write(struct.pack("<I", len(t))); f.write(struct.pack(f"<{len(t)}I", *t))

def time_it(name, fn, reps=3):
    best = None
    for _ in range(reps):
        t = time.perf_counter(); r = fn(); dt = time.perf_counter() - t
        best = dt if best is None or dt < best else best
    return r, best

write_docs(out / "docs.bin", docs)
(out / "rust32k.pattern").write_text(GPT4)
results = {}

# 1. cl100k_base (GPT-4's vocabulary, 100,256 tokens; specials not used)
enc = tiktoken.get_encoding("cl100k_base")
(out / "cl100k.pattern").write_text(enc._pat_str)   # cl100k's own split pattern, not quite nanochat's
write_ranks(out / "cl100k.ranks", enc._mergeable_ranks)
toks, dt = time_it("tiktoken", lambda: [enc.encode_ordinary(d) for d in docs])
write_tokens(out / "cl100k.tokens", toks)
n = sum(map(len, toks))
results["cl100k tiktoken 1 thread"] = (n, dt)

# 2. A 32,768-token vocabulary trained by rustbpe on the corpus (nanochat's size), the same ranks in tiktoken
t = time.perf_counter()
tok = rustbpe.Tokenizer(); tok.train_from_iterator(iter(docs), 32768, pattern=GPT4)
print(f"rustbpe training: {time.perf_counter()-t:.1f} s")
ranks = {bytes(k): v for k, v in tok.get_mergeable_ranks()}
write_ranks(out / "rust32k.ranks", ranks)
enc32 = tiktoken.Encoding(name="rust32k", pat_str=GPT4, mergeable_ranks=ranks, special_tokens={})
toks32, dt = time_it("tiktoken", lambda: [enc32.encode_ordinary(d) for d in docs])
write_tokens(out / "rust32k.tokens", toks32)
n32 = sum(map(len, toks32))
results["rust32k tiktoken 1 thread"] = (n32, dt)
rtoks, dt = time_it("rustbpe", lambda: [tok.encode(d) for d in docs])
assert rtoks == toks32, "rustbpe and tiktoken agree on every document"
results["rust32k rustbpe encode (per document)"] = (n32, dt)

with open(out / "python_results.txt", "w") as f:
    for k, (n, dt) in results.items():
        line = f"{k:45s} {n:>10d} tokens {dt*1000:9.1f} ms {n/dt/1e6:7.2f} M tokens/s {total/dt/1e6:7.1f} MB/s"
        print(line); f.write(line + "\n")
