//
//  long_merge_check.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  long_merge against snicholls::ml::bpe_encode_piece: the same tokens on every piece of the corpus and on
//  random pieces over a tiny vocabulary (where ties are common), then the time of each by piece length, to
//  find where the heap starts to pay, and on long pathological pieces.
//
//      long_merge_check <data dir> <vocab>
//

#include "tok/encoder.hpp"
#include "tok/long_merge.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace tk = snicholls::tokenizer;
namespace ml = snicholls::ml;

namespace {

// compile time: both merges agree on a tiny vocabulary
constexpr std::array<std::pair<std::string_view, std::uint32_t>, 5> tiny{{{"a", 0}, {"b", 1}, {"ab", 2}, {"abab", 3}, {"ba", 4}}};
constexpr auto tiny_rank = [](std::string_view s) {
    for (const auto& [k, v] : tiny)
        if (k == s)
            return v;
    return ml::bpe_no_rank;
};
constexpr bool agree(std::string_view s)
{
    std::array<std::uint32_t, 16> a{}, b{};
    std::array<ml::bpe_part, 81> sc{};
    const std::size_t na = ml::bpe_encode_piece(s, tiny_rank, a, sc).unwrap();
    const std::size_t nb = tk::long_merge(s, tiny_rank, b, sc).unwrap();
    if (na != nb)
        return false;
    for (std::size_t i = 0; i < na; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
static_assert(agree("a") && agree("ab") && agree("abab") && agree("ababab") && agree("bababa") && agree("aabbab"));

std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
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
template <class F>
double best(int reps, F&& f)
{
    double b = 1e30;
    for (int r = 0; r < reps; ++r) {
        const auto t = std::chrono::steady_clock::now();
        f();
        b = std::min(b, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
    }
    return b;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: long_merge_check <data dir> <vocab>\n");
        return 2;
    }
    const std::string dir = argv[1], name = argv[2];
    const std::string raw = slurp(dir + "/docs.bin");
    std::vector<std::string_view> docs;
    {
        const char* p = raw.data();
        std::uint32_t n;
        std::memcpy(&n, p, 4);
        p += 4;
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t l;
            std::memcpy(&l, p, 4);
            p += 4;
            docs.emplace_back(p, l);
            p += l;
        }
    }
    std::vector<std::pair<std::string, std::uint32_t>> kv;
    {
        std::istringstream in(slurp(dir + "/" + name + ".ranks"));
        std::string b;
        std::uint32_t r;
        while (in >> b >> r)
            kv.emplace_back(unbase64(b), r);
    }
    const tk::vocab v(kv);
    const auto pat = name == "cl100k" ? tk::split_pattern::cl100k : tk::split_pattern::gpt4;

    // The pieces a merge is run on: those that are not one token
    std::vector<std::string_view> multi;
    for (const auto d : docs)
        tk::split(d, pat, [&](std::string_view s) {
            if (v(s) == tk::no_rank)
                multi.push_back(s);
        });

    std::vector<std::uint32_t> a(1 << 22), b(1 << 22);
    std::vector<ml::bpe_part> sc(tk::long_merge_scratch_size(1 << 22));
    int failures = 0;
    std::size_t differ = 0;
    for (const auto s : multi) {
        const std::size_t na = ml::bpe_encode_piece(s, v, std::span(a.data(), s.size()), std::span(sc)).unwrap();
        const std::size_t nb = tk::long_merge(s, v, std::span(b.data(), s.size()), std::span(sc)).unwrap();
        differ += na != nb || !std::equal(a.begin(), a.begin() + na, b.begin());
    }
    std::printf("%s: %zu merged pieces of the corpus, %zu differ\n", name.c_str(), multi.size(), differ);
    failures += differ != 0;

    // Random pieces over {a, b, c}, with a vocabulary of every string of up to 4 of them in a random rank order:
    // ties between equal pairs at different places are everywhere, which is where leftmost-first matters
    {
        std::mt19937_64 rng(20261111);
        std::vector<std::pair<std::string, std::uint32_t>> small;
        std::vector<std::string> all{""};
        for (int len = 1; len <= 4; ++len) {
            std::vector<std::string> next;
            for (const auto& s : all)
                if (static_cast<int>(s.size()) == len - 1)
                    for (const char c : std::string("abc"))
                        next.push_back(s + c);
            all.insert(all.end(), next.begin(), next.end());
        }
        all.erase(all.begin());
        std::vector<std::uint32_t> ranks(all.size());
        for (std::uint32_t i = 0; i < ranks.size(); ++i)
            ranks[i] = i;
        std::size_t bad = 0, cases = 0;
        for (int vocab_round = 0; vocab_round < 50; ++vocab_round) {
            std::shuffle(ranks.begin() + 3, ranks.end(), rng);   // single bytes keep low ranks
            small.clear();
            for (std::size_t i = 0; i < all.size(); ++i)
                if (all[i].size() == 1 || rng() % 3 != 0)   // drop a third of the longer strings
                    small.emplace_back(all[i], ranks[i]);
            const tk::vocab sv(small);
            for (int t = 0; t < 2000; ++t) {
                std::string s(1 + rng() % 64, 'a');
                for (char& c : s)
                    c = "abc"[rng() % 3];
                const std::size_t na = ml::bpe_encode_piece(s, sv, std::span(a.data(), s.size()), std::span(sc)).unwrap();
                const std::size_t nb = tk::long_merge(s, sv, std::span(b.data(), s.size()), std::span(sc)).unwrap();
                bad += na != nb || !std::equal(a.begin(), a.begin() + na, b.begin());
                ++cases;
            }
        }
        std::printf("random pieces over a 3-letter alphabet: %zu cases, %zu differ\n", cases, bad);
        failures += bad != 0;
    }

    // Time by length: the corpus's merged pieces in buckets
    std::map<std::size_t, std::vector<std::string_view>> buckets;
    for (const auto s : multi) {
        const std::size_t len = s.size();
        const std::size_t key = len <= 8 ? len : len <= 16 ? 16 : len <= 32 ? 32 : len <= 64 ? 64 : 65;
        buckets[key].push_back(s);
    }
    std::printf("\n  %-10s %8s %14s %14s %8s\n", "length", "pieces", "bpe ns/piece", "heap ns/piece", "heap/bpe");
    for (const auto& [key, ps] : buckets) {
        const int reps = 7;
        volatile std::size_t sink = 0;
        const double tq = best(reps, [&] {
            std::size_t c = 0;
            for (const auto s : ps)
                c += ml::bpe_encode_piece(s, v, std::span(a.data(), s.size()), std::span(sc)).unwrap();
            sink = c;
        });
        const double th = best(reps, [&] {
            std::size_t c = 0;
            for (const auto s : ps)
                c += tk::long_merge(s, v, std::span(b.data(), s.size()), std::span(sc)).unwrap();
            sink = c;
        });
        const std::string label = key <= 8 ? std::to_string(key) : key == 65 ? "65+" : "<=" + std::to_string(key);
        std::printf("  %-10s %8zu %14.1f %14.1f %8.2f\n", label.c_str(), ps.size(), tq * 1e9 / ps.size(), th * 1e9 / ps.size(), th / tq);
    }

    // Long pathological pieces: the split keeps runs of spaces, of punctuation and of letters whole
    std::printf("\n  %-34s %10s %12s %12s %10s\n", "piece", "bytes", "bpe ms", "heap ms", "speedup");
    std::mt19937_64 rng(7);
    for (const std::size_t n : {std::size_t{48}, std::size_t{64}, std::size_t{96}, std::size_t{128}, std::size_t{192},
                                std::size_t{256}, std::size_t{512}, std::size_t{1} << 10, std::size_t{1} << 14,
                                std::size_t{1} << 17}) {
        std::string letters(n, 'a');
        for (char& c : letters)
            c = static_cast<char>('a' + rng() % 26);
        std::string punct(n, '!');
        for (char& c : punct)
            c = "!#$%&*+-/<=>?@^_|~"[rng() % 18];
        for (const auto& [what, s] : {std::pair<const char*, std::string>{"spaces", std::string(n, ' ')},
                                      {"random lowercase letters", letters},
                                      {"random punctuation", punct}}) {
            std::size_t na = 0, nb = 0;
            const int reps = n > (1u << 15) ? 1 : n > 1024 ? 3 : 200;
            const double tq = best(reps, [&] { na = ml::bpe_encode_piece(s, v, std::span(a.data(), s.size()), std::span(sc)).unwrap(); });
            const double th = best(reps, [&] { nb = tk::long_merge(s, v, std::span(b.data(), s.size()), std::span(sc)).unwrap(); });
            const bool same = na == nb && std::equal(a.begin(), a.begin() + na, b.begin());
            failures += !same;
            std::printf("  %-34s %10zu %12.3f %12.3f %9.1fx%s\n", what, n, tq * 1e3, th * 1e3, tq / th, same ? "" : "  DIFFER");
        }
    }
    std::printf("\n%s\n", failures ? "FAILED" : "all agree");
    return failures ? 1 : 0;
}
