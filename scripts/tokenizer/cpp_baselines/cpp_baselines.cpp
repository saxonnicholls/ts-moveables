// cpp_baselines.cpp
// TSMoveables
//
// Copyright 2010-2026 Saxon Herschel Nicholls
//
// Native C++ baselines on prepare.py's data, no Python: quicktok (dmatth1/quicktok) and TokenDagger
// (M4THYOU/TokenDagger), each checked against tiktoken's expected tokens on every document before timing.
//   cpp_baselines <data dir> <quicktok data dir> [--threads 1,2,4,8,16,32] [--reps 5]
// One-thread rows call each library's single-text encode in a loop over the documents. Batch rows use
// quicktok's own encode_batch(texts, threads); TokenDagger has no native batch call (its Python
// encode_batch is a ThreadPoolExecutor over encode with the GIL released), so its batch rows are the same
// thing without Python: N threads claiming documents from a shared cursor. Both call CoreBPE::encode with
// no allowed specials, the call its Python encode and encode_batch make: it looks each piece up whole
// before merging, which its encode_ordinary does not, and is the faster of the two.
// Load time is not timed. Output lines "RESULT<TAB>vocab<TAB>library<TAB>threads<TAB>seconds<TAB>bytes
// <TAB>tokens<TAB>differing documents", as rust_baselines prints them.
#include <quicktok.hpp>
#include <tiktoken.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::uint32_t read_u32(const char*& p)
{
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
}

std::string unbase64(const std::string& in)
{
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (const char c : in) {
        const auto pos = chars.find(c);
        if (pos == std::string::npos)
            break;
        val = (val << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

template <class F> double best_of(int reps, F&& f)
{
    double b = 1e30;
    for (int i = 0; i < reps; ++i) {
        const auto t = std::chrono::steady_clock::now();
        f();
        b = std::min(b, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
    }
    return b;
}

template <class T, class F> std::vector<std::vector<T>> threaded(std::size_t n, unsigned threads, F&& one)
{
    std::vector<std::vector<T>> out(n);
    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (std::size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;)
            out[i] = one(i);
    };
    std::vector<std::thread> ths;
    for (unsigned t = 1; t < threads; ++t)
        ths.emplace_back(work);
    work();
    for (auto& t : ths)
        t.join();
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: cpp_baselines <data dir> <quicktok data dir> [--threads 1,2,4] [--reps n]\n");
        return 2;
    }
    const std::string dir = argv[1], qt_dir = argv[2];
    std::vector<unsigned> thread_counts{1, 2, 4, 8, 16, 32};
    int reps = 5;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--reps" && i + 1 < argc)
            reps = std::atoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) {
            thread_counts.clear();
            std::stringstream s(argv[++i]);
            std::string t;
            while (std::getline(s, t, ','))
                thread_counts.push_back(static_cast<unsigned>(std::stoul(t)));
        }
    }

    const std::string docs_raw = slurp(dir + "/docs.bin");
    std::vector<std::string_view> views;
    std::vector<std::string> docs;
    {
        const char* p = docs_raw.data();
        const std::uint32_t n = read_u32(p);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t len = read_u32(p);
            views.emplace_back(p, len);
            docs.emplace_back(p, len);
            p += len;
        }
    }
    std::size_t bytes = 0;
    for (const auto d : views)
        bytes += d.size();
    const std::size_t n = docs.size();

    for (const std::string name : {"cl100k", "rust32k"}) {
        std::vector<std::vector<std::uint32_t>> expected;
        {
            const std::string raw = slurp(dir + "/" + name + ".tokens");
            const char* p = raw.data();
            const std::uint32_t m = read_u32(p);
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t len = read_u32(p);
                expected.emplace_back(len);
                std::memcpy(expected.back().data(), p, 4ull * len);
                p += 4ull * len;
            }
        }
        std::size_t tokens = 0;
        for (const auto& e : expected)
            tokens += e.size();
        std::printf("%s: %zu documents, %.1f MB, %zu tokens\n", name.c_str(), n, static_cast<double>(bytes) / 1e6, tokens);

        auto differ = [&](const auto& got) {
            std::size_t wrong = 0;
            for (std::size_t i = 0; i < n; ++i)
                wrong += !(got[i].size() == expected[i].size() &&
                           std::equal(got[i].begin(), got[i].end(), expected[i].begin(),
                                      [](auto a, std::uint32_t b) { return static_cast<std::uint32_t>(a) == b; }));
            return wrong;
        };
        auto row = [&](const std::string& lib, unsigned t, double s, std::size_t wrong) {
            std::printf("  %-30s %3u threads %9.1f ms %8.2f M tok/s %8.1f MB/s  %zu docs differ\n", lib.c_str(), t, s * 1e3,
                        static_cast<double>(tokens) / s / 1e6, static_cast<double>(bytes) / s / 1e6, wrong);
            std::printf("RESULT\t%s\t%s\t%u\t%.6f\t%zu\t%zu\t%zu\n", name.c_str(), lib.c_str(), t, s, bytes, tokens, wrong);
            std::fflush(stdout);
        };

        // quicktok: built-in cl100k_base; it cannot load a raw .ranks file with another pattern
        if (name == "cl100k") {
            const auto qt = quicktok::Tokenizer::load_dir(qt_dir, "cl100k_base");
            std::vector<std::vector<std::uint32_t>> got;
            const double s1 = best_of(reps, [&] {
                got.assign(n, {});
                for (std::size_t i = 0; i < n; ++i)
                    got[i] = qt.encode(views[i]);
            });
            row("quicktok 0.4.0", 1, s1, differ(got));
            for (const unsigned t : thread_counts) {
                const double s = best_of(reps, [&] { got = qt.encode_batch(views, t); });
                row("quicktok 0.4.0 batch", t, s, differ(got));
            }
        }

        // TokenDagger: raw ranks and the vocabulary's own pattern, as tiktoken.Encoding takes them
        {
            std::vector<VocabItem> vocab;
            std::istringstream in(slurp(dir + "/" + name + ".ranks"));
            std::string b;
            int r;
            while (in >> b >> r) {
                const std::string bytes_ = unbase64(b);
                vocab.push_back(VocabItem{r, std::vector<unsigned char>(bytes_.begin(), bytes_.end()), bytes_});
            }
            const std::string pat = [&] {
                std::string p = slurp(dir + "/" + name + ".pattern");
                while (!p.empty() && (p.back() == '\n' || p.back() == '\r'))
                    p.pop_back();
                return p;
            }();
            tiktoken::CoreBPE td(pat, vocab, {});
            const emhash8::HashSet<std::string> none;
            std::vector<std::vector<int>> got;
            const double s1 = best_of(reps, [&] {
                got.assign(n, {});
                for (std::size_t i = 0; i < n; ++i)
                    got[i] = td.encode(docs[i], none).first;
            });
            row("TokenDagger 0.1.1", 1, s1, differ(got));
            for (const unsigned t : thread_counts) {
                const double s = best_of(reps, [&] { got = threaded<int>(n, t, [&](std::size_t i) { return td.encode(docs[i], none).first; }); });
                row("TokenDagger 0.1.1 batch", t, s, differ(got));
            }
        }
    }
    return 0;
}
