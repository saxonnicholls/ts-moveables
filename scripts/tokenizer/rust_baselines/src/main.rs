// main.rs
// TSMoveables
//
// Copyright 2010-2026 Saxon Herschel Nicholls
//
// Native Rust baselines, no Python, each checked against tiktoken's expected tokens on every document:
// tiktoken-rs CoreBPE (tiktoken's own Rust core), GitHub's bpe-openai, Hugging Face tokenizers (on a
// cl100k tokenizer.json), kitoken, splintr, goliajp's tiktoken, toktok and tokie.
//
// One process measures one thread count, the size of rayon's global pool, so RAYON_NUM_THREADS picks it:
//   RAYON_NUM_THREADS=8 cargo run --release -- <data dir> [--json tokenizer.json] [--reps 5] [--single]
// --single adds the one-thread rows (each library's own single-text call in a loop over the documents).
// The batch rows use each library's own batch call where it has one, with the thread count set to the
// same number; otherwise rayon over the documents. Load time is not timed.
// Output: a readable row per measurement, plus a line "RESULT<TAB>vocab<TAB>library<TAB>threads<TAB>
// seconds<TAB>bytes<TAB>tokens<TAB>differing documents" that run_comparison.sh collects.
use base64::Engine;
use rayon::prelude::*;
use std::time::Instant;

fn read_u32(b: &[u8], at: &mut usize) -> u32 {
    let v = u32::from_le_bytes(b[*at..*at + 4].try_into().unwrap());
    *at += 4;
    v
}

fn best<T>(reps: usize, f: impl FnMut() -> T) -> (T, f64) {
    let (r, b, _) = best_and_first(reps, f);
    (r, b)
}

/// Best of `reps`, and the first rep alone: the cold time, before any cache the library keeps has seen the
/// corpus. A library that memoises pieces across calls is best-of-N warm; the first pass is what one pass over
/// new text costs.
fn best_and_first<T>(reps: usize, mut f: impl FnMut() -> T) -> (T, f64, f64) {
    let mut b = f64::MAX;
    let mut first = 0.0;
    let mut r = None;
    for i in 0..reps {
        let t = Instant::now();
        let x = f();
        let s = t.elapsed().as_secs_f64();
        if i == 0 {
            first = s;
        }
        b = b.min(s);
        r = Some(x);
    }
    (r.unwrap(), b, first)
}

/// The pattern with its possessive quantifiers (?+ ++ *+ {m,n}+) made greedy
fn greedy_pattern(p: &str) -> String {
    let c: Vec<char> = p.chars().collect();
    let mut out = String::new();
    let mut i = 0;
    while i < c.len() {
        if c[i] == '\\' && i + 1 < c.len() {
            out.push(c[i]);
            out.push(c[i + 1]);
            i += 2;
            continue;
        }
        out.push(c[i]);
        if c[i] == '{' && !(i > 0 && (c[i - 1] == 'p' || c[i - 1] == 'P')) {
            // a counted repetition {m,n}: copy it, then drop a possessive + after it
            let mut j = i + 1;
            while j < c.len() && c[j] != '}' {
                out.push(c[j]);
                j += 1;
            }
            out.push('}');
            i = j + 1;
            if i < c.len() && c[i] == '+' {
                i += 1;
            }
            continue;
        }
        if matches!(c[i], '?' | '+' | '*') && i + 1 < c.len() && c[i + 1] == '+' && !(i > 0 && c[i - 1] == '(') {
            i += 2;
            continue;
        }
        i += 1;
    }
    out
}

type Single<'a> = Box<dyn Fn(&str) -> Vec<u32> + Sync + 'a>;
type Batch<'a> = Box<dyn Fn(&[&str], usize) -> Vec<Vec<u32>> + Sync + 'a>;

/// One library on one vocabulary: its single-text call, and its own batch call if it has one.
struct Lib<'a> {
    name: String,
    single: Single<'a>,
    batch: Option<Batch<'a>>,
    /// The batch call ignores the thread count (it always uses every core)
    batch_threads_fixed: bool,
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let dir = args.get(1).expect("data dir").clone();
    let mut json = None;
    let mut reps = 5usize;
    let mut single = false;
    let mut only: Option<Vec<String>> = None;
    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => { json = Some(args[i + 1].clone()); i += 1; }
            "--reps" => { reps = args[i + 1].parse().unwrap(); i += 1; }
            "--only" => { only = Some(args[i + 1].split(',').map(String::from).collect()); i += 1; }
            "--single" => single = true,
            a => panic!("unknown argument {a}"),
        }
        i += 1;
    }
    let raw = std::fs::read(format!("{dir}/docs.bin")).unwrap();
    let mut at = 0;
    let n = read_u32(&raw, &mut at);
    let mut docs = Vec::new();
    for _ in 0..n {
        let l = read_u32(&raw, &mut at) as usize;
        docs.push(std::str::from_utf8(&raw[at..at + l]).unwrap().to_string());
        at += l;
    }
    let refs: Vec<&str> = docs.iter().map(|d| d.as_str()).collect();
    let byte_refs: Vec<&[u8]> = docs.iter().map(|d| d.as_bytes()).collect();
    let bytes: usize = docs.iter().map(|d| d.len()).sum();
    let threads = rayon::current_num_threads();
    let hw = std::thread::available_parallelism().map(|p| p.get()).unwrap_or(1);

    for name in ["cl100k", "rust32k"] {
        let ranks_path = format!("{dir}/{name}.ranks");
        let mut ranks: rustc_hash::FxHashMap<Vec<u8>, u32> = Default::default();
        for line in std::fs::read_to_string(&ranks_path).unwrap().lines() {
            let mut it = line.split(' ');
            let b = base64::engine::general_purpose::STANDARD.decode(it.next().unwrap()).unwrap();
            ranks.insert(b, it.next().unwrap().parse::<u32>().unwrap());
        }
        let pat = std::fs::read_to_string(format!("{dir}/{name}.pattern")).unwrap();
        let pat = pat.trim_end().to_string();
        let tb = std::fs::read(format!("{dir}/{name}.tokens")).unwrap();
        let mut at = 0;
        let mut expected: Vec<Vec<u32>> = Vec::new();
        let n = read_u32(&tb, &mut at);
        for _ in 0..n {
            let l = read_u32(&tb, &mut at) as usize;
            expected.push((0..l).map(|_| read_u32(&tb, &mut at)).collect());
        }
        let tokens: usize = expected.iter().map(|e| e.len()).sum();
        println!("{name}: {} documents, {:.1} MB, {tokens} tokens, {threads} threads", docs.len(), bytes as f64 / 1e6);

        // Load every library (not timed). A library that cannot load this vocabulary is left out.
        let mut libs: Vec<Lib> = Vec::new();

        let core = tiktoken_rs::CoreBPE::new(ranks.clone(), rustc_hash::FxHashMap::default(), &pat).unwrap();
        libs.push(Lib { name: "tiktoken-rs 0.12.1".into(), single: Box::new(move |d| core.encode_ordinary(d)), batch: None, batch_threads_fixed: false });

        if name == "cl100k" {
            // bpe-openai ships cl100k only; it cannot load a raw .ranks file with a pattern
            let bpe = bpe_openai::cl100k_base();
            libs.push(Lib { name: "bpe-openai 0.3.2".into(), single: Box::new(move |d| bpe.encode(d)), batch: None, batch_threads_fixed: false });

            // Hugging Face tokenizers loads a tokenizer.json (vocab + merges), not raw ranks
            if let Some(j) = &json {
                let hf = tokenizers::Tokenizer::from_file(j).unwrap();
                let hf2 = hf.clone();
                libs.push(Lib {
                    name: "tokenizers (HF) 0.23.2".into(),
                    single: Box::new(move |d| hf.encode_fast(d, false).unwrap().get_ids().to_vec()),
                    batch: Some(Box::new(move |ds, _| {
                        hf2.encode_batch_fast(ds.to_vec(), false).unwrap().iter().map(|e| e.get_ids().to_vec()).collect()
                    })),
                    batch_threads_fixed: false,
                });
                let tk = tokie::Tokenizer::from_json(j).unwrap();
                let tk = std::sync::Arc::new(tk);
                let tk2 = tk.clone();
                libs.push(Lib {
                    name: "tokie 0.1.4".into(),
                    single: Box::new(move |d| tk.encode_ids(d, false)),
                    batch: Some(Box::new(move |ds, _| {
                        let (ids, lens) = tk2.encode_batch_flat(ds, false);
                        let mut out = Vec::with_capacity(lens.len());
                        let mut at = 0usize;
                        for l in lens {
                            out.push(ids[at..at + l as usize].to_vec());
                            at += l as usize;
                        }
                        out
                    })),
                    batch_threads_fixed: true,
                });
            }

            // goliajp's tiktoken and toktok ship their vocabularies built in; neither loads raw ranks
            let gt = tiktoken::get_encoding("cl100k_base").unwrap();
            libs.push(Lib { name: "tiktoken (goliajp) 4.1.2".into(), single: Box::new(move |d| gt.encode(d)), batch: None, batch_threads_fixed: false });

            let tt = toktok::Tokenizer::builtin("cl100k_base").unwrap();
            let tt = std::sync::Arc::new(tt);
            let tt2 = tt.clone();
            let brefs = byte_refs.clone();
            libs.push(Lib {
                name: "toktok-rs 0.3.0".into(),
                single: Box::new(move |d| tt.encode(d.as_bytes())),
                batch: Some(Box::new(move |_, t| tt2.encode_batch(&brefs, t, false))),
                batch_threads_fixed: false,
            });
        }

        // kitoken loads raw tiktoken ranks; it infers cl100k's split from the vocabulary size, and for any
        // other vocabulary we give it the pattern, as tiktoken-rs is given it
        {
            let mut def = kitoken::Definition::from_tiktoken_file(&ranks_path).unwrap();
            if name != "cl100k" {
                def.config.split = vec![kitoken::Split::Pattern {
                    pattern: kitoken::Regex::new(&pat).unwrap().into(),
                    behavior: kitoken::SplitBehavior::Isolate,
                }];
            }
            let kt = kitoken::Kitoken::from_definition(def).unwrap();
            libs.push(Lib { name: "kitoken 0.11.1".into(), single: Box::new(move |d| kt.encode(d, false).unwrap()), batch: None, batch_threads_fixed: false });
        }

        // splintr loads raw ranks with a pattern; its encode_batch runs on rayon's global pool. Its regex
        // engine refuses possessive quantifiers, so it gets the pattern with them made greedy, which is how
        // splintr writes cl100k's own pattern (CL100K_BASE_PATTERN)
        {
            let greedy = greedy_pattern(&pat);
            if name == "cl100k" {
                assert_eq!(greedy, splintr::CL100K_BASE_PATTERN, "greedy cl100k pattern is splintr's own");
            }
            let mut enc: splintr::FxHashMap<Vec<u8>, u32> = Default::default();
            for (k, v) in &ranks {
                enc.insert(k.clone(), *v);
            }
            let specials: splintr::FxHashMap<String, u32> = Default::default();
            let sp = std::sync::Arc::new(splintr::Tokenizer::new(enc, specials, &greedy).unwrap());
            let sp2 = sp.clone();
            let owned: Vec<String> = docs.clone();
            libs.push(Lib {
                name: "splintr 0.22.2".into(),
                single: Box::new(move |d| sp.encode_ordinary(d)),
                batch: Some(Box::new(move |_, _| sp2.encode_batch(&owned))),
                batch_threads_fixed: false,
            });
        }

        let row = |lib: &str, t: usize, s: f64, differ: usize| {
            println!("  {lib:30} {t:>3} threads {:9.1} ms {:8.2} M tok/s {:8.1} MB/s  {differ} docs differ",
                     s * 1e3, tokens as f64 / s / 1e6, bytes as f64 / s / 1e6);
            println!("RESULT\t{name}\t{lib}\t{t}\t{s:.6}\t{bytes}\t{tokens}\t{differ}");
        };
        let differ = |got: &Vec<Vec<u32>>| got.iter().zip(&expected).filter(|(a, b)| a != b).count() + expected.len().abs_diff(got.len());

        for lib in &libs {
            if let Some(o) = &only {
                if !o.iter().any(|x| lib.name.starts_with(x.as_str())) {
                    continue;
                }
            }
            if single {
                // The single-text loop runs first, so its first rep is this library's first look at the corpus
                let (got, s, cold) = best_and_first(reps, || refs.iter().map(|d| (lib.single)(d)).collect::<Vec<_>>());
                row(&lib.name, 1, s, differ(&got));
                row(&format!("{} cold", lib.name), 1, cold, differ(&got));
            }
            if lib.batch_threads_fixed && threads != hw {
                continue;
            }
            let (got, s) = match &lib.batch {
                Some(b) => best(reps, || b(&refs, threads)),
                None => best(reps, || refs.par_iter().map(|d| (lib.single)(d)).collect::<Vec<_>>()),
            };
            row(&format!("{} batch", lib.name), threads, s, differ(&got));
        }
    }
}
