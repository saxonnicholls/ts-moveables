//
//  tokenizer_bench.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Byte-level BPE across every core, checked token for token against tiktoken, and timed.
//
//      tokenizer_bench <data dir> <vocab> [--threads 1,2,4,...] [--markdown]
//
//  <data dir> is what scripts/tokenizer/prepare.py writes: docs.bin (the corpus), <vocab>.ranks (tiktoken's
//  base64 format), <vocab>.pattern (the vocabulary's own split pattern) and <vocab>.tokens (tiktoken's tokens
//  for every document). Before anything is timed, every document is encoded serially and in parallel and must
//  equal tiktoken's tokens, and the whole corpus as one text, cut at find_boundary positions across the pool,
//  must equal its serial encoding. Any difference exits 1.
//

#include "tok/encoder.hpp"

#include "concurrent/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tk = snicholls::tokenizer;

namespace {

std::string slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(2);
    }
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string unbase64(std::string_view in)
{
    static constexpr std::string_view a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        if (c == '=')
            break;
        acc = (acc << 6) | static_cast<std::uint32_t>(a.find(c));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    return out;
}

std::uint32_t read_u32(const char*& p)
{
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
}

tk::split_pattern pattern_of(std::string pat)
{
    while (!pat.empty() && (pat.back() == '\n' || pat.back() == '\r'))
        pat.pop_back();
    static const std::string gpt4 =
        R"('(?i:[sdmt]|ll|ve|re)|[^\r\n\p{L}\p{N}]?+\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]++[\r\n]*|\s*[\r\n]|\s+(?!\S)|\s+)";
    static const std::string cl100k =
        R"('(?i:[sdmt]|ll|ve|re)|[^\r\n\p{L}\p{N}]?+\p{L}++|\p{N}{1,3}+| ?[^\s\p{L}\p{N}]++[\r\n]*+|\s++$|\s*[\r\n]|\s+(?!\S)|\s)";
    if (pat == gpt4)
        return tk::split_pattern::gpt4;
    if (pat == cl100k)
        return tk::split_pattern::cl100k;
    std::fprintf(stderr, "unsupported split pattern: %s\n", pat.c_str());
    std::exit(2);
}

template <typename F>
double best_of(int reps, F&& fn)
{
    double best = 1e30;
    for (int r = 0; r < reps; ++r) {
        const auto t = std::chrono::steady_clock::now();
        fn();
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
    }
    return best;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: tokenizer_bench <data dir> <vocab> [--threads 1,2,4] [--reps n] [--markdown]\n");
        return 2;
    }
    const std::string dir = argv[1], name = argv[2];
    std::vector<std::size_t> thread_counts;
    bool markdown = false;
    int reps = 5;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--markdown")
            markdown = true;
        else if (a == "--reps" && i + 1 < argc)
            reps = std::atoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) {
            std::stringstream s(argv[++i]);
            std::string t;
            while (std::getline(s, t, ','))
                thread_counts.push_back(std::stoul(t));
        }
    }
    const std::size_t hw = std::max(1u, std::thread::hardware_concurrency());
    if (thread_counts.empty())
        for (std::size_t t = 1; t <= hw; t *= 2)
            thread_counts.push_back(t);
    if (thread_counts.back() != hw && thread_counts.back() < hw)
        thread_counts.push_back(hw);

    // The corpus, the vocabulary and tiktoken's tokens
    const std::string docs_raw = slurp(dir + "/docs.bin");
    std::vector<std::string_view> docs;
    {
        const char* p = docs_raw.data();
        const std::uint32_t n = read_u32(p);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t len = read_u32(p);
            docs.emplace_back(p, len);
            p += len;
        }
    }
    std::size_t bytes = 0;
    for (const auto d : docs)
        bytes += d.size();

    std::vector<std::pair<std::string, std::uint32_t>> kv;
    {
        std::istringstream in(slurp(dir + "/" + name + ".ranks"));
        std::string b;
        std::uint32_t r;
        while (in >> b >> r)
            kv.emplace_back(unbase64(b), r);
    }
    const tk::encoder enc(tk::vocab(kv), pattern_of(slurp(dir + "/" + name + ".pattern")));

    std::vector<std::vector<std::uint32_t>> expected;
    {
        const std::string raw = slurp(dir + "/" + name + ".tokens");
        const char* p = raw.data();
        const std::uint32_t n = read_u32(p);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t len = read_u32(p);
            expected.emplace_back(len);
            std::memcpy(expected.back().data(), p, 4ull * len);
            p += 4ull * len;
        }
    }
    if (expected.size() != docs.size()) {
        std::fprintf(stderr, "%zu documents but %zu token lists\n", docs.size(), expected.size());
        return 2;
    }
    std::size_t tokens = 0;
    for (const auto& e : expected)
        tokens += e.size();

    // Accuracy, before any timing
    int failures = 0;
    auto check = [&](const char* what, const std::vector<std::vector<std::uint32_t>>& got) {
        std::size_t wrong = 0;
        for (std::size_t i = 0; i < docs.size(); ++i)
            if (got[i] != expected[i]) {
                if (++wrong <= 3) {
                    std::size_t k = 0;
                    while (k < got[i].size() && k < expected[i].size() && got[i][k] == expected[i][k])
                        ++k;
                    std::fprintf(stderr, "%s: document %zu differs at token %zu (%zu tokens, tiktoken %zu)\n", what, i, k,
                                 got[i].size(), expected[i].size());
                }
            }
        if (wrong)
            ++failures;
        std::printf("check %-34s %zu of %zu documents differ from tiktoken\n", what, wrong, docs.size());
    };
    {
        std::vector<std::vector<std::uint32_t>> serial;
        for (const auto d : docs)
            serial.push_back(enc.encode(d));
        check("serial", serial);
        for (const auto& e : serial)
            for (const std::uint32_t t : e)
                if (enc.vocab().token(t).empty())
                    ++failures;
        std::size_t round_trip = 0;
        for (std::size_t i = 0; i < docs.size(); ++i)
            round_trip += enc.decode(serial[i]) != docs[i];
        std::printf("check %-34s %zu documents do not decode to their bytes\n", "decode", round_trip);
        failures += round_trip != 0;
    }
    snicholls::work_stealing_task_pool pool(hw);
    check("parallel batch, 4 KB chunks", enc.encode_batch(docs, pool, 4096));
    check("parallel batch, default chunks", enc.encode_batch(docs, pool));

    // The corpus as one text: cut across the pool, it must give its serial tokens
    std::string all;
    all.reserve(bytes);
    for (const auto d : docs)
        all += d;
    const std::vector<std::uint32_t> all_serial = enc.encode(all);
    for (const std::size_t ch : {std::size_t{1} << 12, std::size_t{1} << 16, std::size_t{0}}) {
        const bool same = enc.encode(all, pool, ch) == all_serial;
        std::printf("check %-34s %s\n", ("one text, chunk " + std::to_string(ch)).c_str(), same ? "same as serial" : "DIFFERS");
        failures += !same;
    }
    if (failures) {
        std::printf("FAILED: %d checks\n", failures);
        return 1;
    }

    // Timing
    std::size_t pieces = 0;
    const double t_split = best_of(reps, [&] {
        std::size_t n = 0;
        for (const auto d : docs)
            tk::split(d, enc.pattern(), [&](std::string_view) { ++n; });
        pieces = n;
    });
    std::vector<std::uint32_t> sink(bytes);
    const double t_serial = best_of(reps, [&] {
        std::uint32_t* at = sink.data();
        for (const auto d : docs)
            at += enc.encode_range(d, 0, d.size(), at);
    });

    auto row = [&](const std::string& what, double s, double base) {
        const double mtok = static_cast<double>(tokens) / s / 1e6, mbs = static_cast<double>(bytes) / s / 1e6;
        if (markdown)
            std::printf("| %s | %.1f | %.2f | %.1f | %.2fx |\n", what.c_str(), s * 1e3, mtok, mbs, base / s);
        else
            std::printf("  %-36s %9.1f ms %8.2f M tok/s %8.1f MB/s %7.2fx\n", what.c_str(), s * 1e3, mtok, mbs, base / s);
    };
    std::printf("\n%s: %zu documents, %.1f MB, %zu pieces, %zu tokens; %zu hardware threads\n", name.c_str(), docs.size(),
                static_cast<double>(bytes) / 1e6, pieces, tokens, hw);
    if (markdown)
        std::printf("\n| | ms | M tokens/s | MB/s | vs serial |\n|---|---:|---:|---:|---:|\n");
    row("split only, 1 thread", t_split, t_serial);
    row("encode, 1 thread", t_serial, t_serial);
    for (const std::size_t t : thread_counts) {
        snicholls::work_stealing_task_pool p(t);
        const double s = best_of(reps, [&] { (void)enc.encode_batch(docs, p); });
        row("batch, " + std::to_string(t) + " workers + caller", s, t_serial);
    }
    for (const std::size_t t : thread_counts) {
        snicholls::work_stealing_task_pool p(t);
        const double s = best_of(reps, [&] { (void)enc.encode(all, p); });
        row("one text, " + std::to_string(t) + " workers + caller", s, t_serial);
    }
    return 0;
}
