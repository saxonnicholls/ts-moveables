// main.rs
// TSMoveables
//
// Copyright 2010-2026 Saxon Herschel Nicholls
//
// Native Rust baselines, no Python: tiktoken-rs CoreBPE (tiktoken's own Rust core) and GitHub's bpe-openai,
// one thread and rayon across documents, each checked against tiktoken's expected tokens.
//   cargo run --release -- <data dir>
use base64::Engine;
use rayon::prelude::*;
use std::time::Instant;

fn read_u32(b: &[u8], at: &mut usize) -> u32 {
    let v = u32::from_le_bytes(b[*at..*at + 4].try_into().unwrap());
    *at += 4;
    v
}

fn best<T>(reps: usize, mut f: impl FnMut() -> T) -> (T, f64) {
    let mut b = f64::MAX;
    let mut r = None;
    for _ in 0..reps {
        let t = Instant::now();
        let x = f();
        b = b.min(t.elapsed().as_secs_f64());
        r = Some(x);
    }
    (r.unwrap(), b)
}

fn main() {
    let dir = std::env::args().nth(1).expect("data dir");
    let raw = std::fs::read(format!("{dir}/docs.bin")).unwrap();
    let mut at = 0;
    let n = read_u32(&raw, &mut at);
    let mut docs = Vec::new();
    for _ in 0..n {
        let l = read_u32(&raw, &mut at) as usize;
        docs.push(std::str::from_utf8(&raw[at..at + l]).unwrap().to_string());
        at += l;
    }
    let bytes: usize = docs.iter().map(|d| d.len()).sum();
    let all: String = docs.concat();
    let threads = rayon::current_num_threads();

    let row = |what: &str, toks: usize, s: f64| {
        println!("  {what:46} {:9.1} ms {:8.2} M tok/s {:8.1} MB/s", s * 1e3, toks as f64 / s / 1e6, bytes as f64 / s / 1e6);
    };

    for name in ["cl100k", "rust32k"] {
        let mut ranks: rustc_hash::FxHashMap<Vec<u8>, u32> = Default::default();
        for line in std::fs::read_to_string(format!("{dir}/{name}.ranks")).unwrap().lines() {
            let mut it = line.split(' ');
            let b = base64::engine::general_purpose::STANDARD.decode(it.next().unwrap()).unwrap();
            ranks.insert(b, it.next().unwrap().parse::<u32>().unwrap());
        }
        let pat = std::fs::read_to_string(format!("{dir}/{name}.pattern")).unwrap();
        let tb = std::fs::read(format!("{dir}/{name}.tokens")).unwrap();
        let mut at = 0;
        let mut expected: Vec<Vec<u32>> = Vec::new();
        let n = read_u32(&tb, &mut at);
        for _ in 0..n {
            let l = read_u32(&tb, &mut at) as usize;
            expected.push((0..l).map(|_| read_u32(&tb, &mut at)).collect());
        }
        let tokens: usize = expected.iter().map(|e| e.len()).sum();
        println!("{name}: {} documents, {:.1} MB, {tokens} tokens, rayon {threads} threads", docs.len(), bytes as f64 / 1e6);

        let core = tiktoken_rs::CoreBPE::new(ranks, rustc_hash::FxHashMap::default(), pat.trim_end()).unwrap();
        let (got, s) = best(3, || docs.iter().map(|d| core.encode_ordinary(d)).collect::<Vec<_>>());
        assert!(got == expected, "tiktoken-rs differs from tiktoken");
        row("tiktoken-rs, 1 thread", tokens, s);
        let (got, s) = best(5, || docs.par_iter().map(|d| core.encode_ordinary(d)).collect::<Vec<_>>());
        assert!(got == expected);
        row(&format!("tiktoken-rs, rayon over documents ({threads})"), tokens, s);
        let (_, s) = best(3, || core.encode_ordinary(&all));
        row("tiktoken-rs, one text", tokens, s);

        if name == "cl100k" {
            let bpe = bpe_openai::cl100k_base();
            let got: Vec<Vec<u32>> = docs.iter().map(|d| bpe.encode(d.as_str())).collect();
            let differ = got.iter().zip(&expected).filter(|(a, b)| a != b).count();
            let (_, s) = best(3, || docs.iter().map(|d| bpe.encode(d.as_str())).collect::<Vec<_>>());
            row(&format!("bpe-openai, 1 thread ({differ} docs differ)"), tokens, s);
            let (_, s) = best(5, || docs.par_iter().map(|d| bpe.encode(d.as_str())).collect::<Vec<_>>());
            row(&format!("bpe-openai, rayon over documents ({threads})"), tokens, s);
            let (_, s) = best(3, || bpe.encode(all.as_str()));
            row("bpe-openai, one text", tokens, s);
        }
    }
}
