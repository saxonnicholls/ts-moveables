//
//  persistent_index_demo.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a full-text index that is built once and never
//  rebuilt: a suffix array in memory-mapped files (mmap/)
//
//      make demo-text-index                      the self-test: build, reopen, search, kill and
//                                                resume, refuse a stale or corrupt index, readers
//      ./build/persistent_index_demo --size 400000000 --dir /big/disk/idx --keep --durable
//      ./build/persistent_index_demo --dir /big/disk/idx query ACGTTTTACG GATTACA
//      ./build/persistent_index_demo --file corpus.txt --dir idx --keep query "the cat"
//
//  ------------------------------------------------------------- the index
//
//  A suffix array lists every position of a text in the sorted order of the
//  suffixes starting there, so every occurrence of a pattern is one
//  contiguous range of it, found by binary search: O(m log n) for a pattern
//  of m bytes, however many times it occurs. Building it is the expensive
//  part: log2(n) rounds of a parallel sort over every position
//  (text_index/suffix_array.hpp, shared with the BWT demo). That is seconds
//  for a few million bases and minutes for a genome.
//
//  Here the build happens once. The index lives in three files, each a
//  mapped_vector with core's checksummed header:
//
//      text.bin     the text and its sentinel
//      sa.bin       the suffix array, int32 per position
//      prefix.bin   where each 2-byte prefix's range starts in sa (65,537
//                   entries), so a search binary-searches a slice, not all of sa
//
//  Each header's type tag is a hash of the source (the DNA seed and size, or
//  the file's path, size and modification time) and of what the file holds.
//  An index of different data is therefore refused (type_mismatch) and
//  rebuilt, never silently used.
//
//  -------------------------------------------- what reopening costs instead
//
//  Reopening is three mmaps and three 64-byte header checks: a few tenths of
//  a millisecond, whatever the size. Nothing is read until a search touches it, and a search
//  touches about log2(n) pages. Several processes searching the same index
//  share one copy in the page cache.
//
//  ------------------------------------------------ a build that resumes
//
//  Each doubling round reads the current ranks and writes the next ones into
//  a second file, so the state before a round survives a kill in the middle
//  of it. The program writes the ranks before the progress record, so a
//  killed process leaves a consistent checkpoint in the page cache with no
//  msync at all; --durable adds an msync of each, in that order, so the
//  checkpoint also survives a power cut (it costs a disk sync a round,
//  milliseconds on a real disk, seconds on Docker Desktop's virtual one).
//  Only the ranks are checkpointed: the order being sorted, and the
//  scan's scratch, are rebuilt on resume, so they live in anonymous memory,
//  where a 32-thread sort does not fight the kernel's writeback of a file. After each round the new ranks are flushed, and then progress.bin
//  records the round. A killed build resumes at its last completed round
//  instead of starting again. prefix.bin is written last; its presence means
//  the index is complete, and the build files are then deleted.
//
//  ------------------------------------------------------------ what it checks
//
//  The self-test exits non-zero unless:
//    - sa is a permutation, and every adjacent pair of suffixes is in order;
//    - every search's count matches a naive scan of the text;
//    - a build killed after round 3 resumes and produces the same sa,
//      byte for byte, as an uninterrupted build;
//    - an index of different data, and one with a corrupted header, are
//      refused;
//    - four reader processes each find the same counts.
//
//  C++20 and POSIX (fork, for the kill and the readers), like mmap/.
//

#include "../../TSMoveables/concurrent/thread_pool.hpp"
#include "../../TSMoveables/mmap/mapped_vector.hpp"
#include "suffix_array.hpp"

#include <snicholls/core/hash/fnv.hpp>
#include <snicholls/core/hash/wyhash.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace snicholls;
namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

namespace {

constexpr char sentinel = '\x01';   // below every byte of the text (which never holds 0 or 1), so it sorts first
constexpr std::size_t prefix_entries = 65536 + 1;

double us_since(clk::time_point t0) { return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); }
double ms_since(clk::time_point t0) { return us_since(t0) / 1000.0; }

int failures = 0;
void check(bool ok, const char* what)
{
    std::printf("    %-66s %s\n", what, ok ? "ok" : "FAILED");
    std::fflush(stdout);
    if (!ok)
        ++failures;
}

void fail_hard(const char* what, const std::error_code& e)
{
    std::fprintf(stderr, "persistent_index_demo: %s: %s\n", what, e.message().c_str());
    std::exit(1);
}

// ------------------------------------------------------------- the source

struct source
{
    std::size_t size = 8'000'000;   // bases of synthetic DNA
    unsigned seed = 1;
    std::string file;               // or a file to index instead

    // Names the data: two sources with the same tag hold the same text
    std::uint64_t tag() const
    {
        std::string id;
        if (file.empty()) {
            id = "dna:" + std::to_string(seed) + ":" + std::to_string(size);
        } else {
            std::error_code ec;
            const auto bytes = fs::file_size(file, ec);
            const auto mtime = static_cast<long long>(fs::last_write_time(file, ec).time_since_epoch().count());
            id = "file:" + fs::absolute(file).string() + ":" + std::to_string(bytes) + ":" + std::to_string(mtime);
        }
        return snicholls::wyhash(id, snicholls::fnv1a_64("persistent_index v1"));
    }
};

std::uint64_t tag_of(const source& src, std::string_view what) { return src.tag() ^ snicholls::fnv1a_64(what); }

// ----------------------------------------------------------------- the index

struct index_files
{
    mapped_vector<char> text;            // with its sentinel
    mapped_vector<std::int32_t> sa;
    mapped_vector<std::uint32_t> prefix;

    std::size_t n() const { return text.size(); }
    std::string_view view() const { return {text.data(), text.size()}; }
};

std::string at(const std::string& dir, const char* name) { return dir + "/" + name; }

// Open a complete index, or say why not. A complete index never grows, so each file reserves only its own size:
// on macOS reserving (and later releasing) the default 64 GiB of address space costs about 0.25 ms a file, which
// would be most of the reopen.
result<index_files, std::error_code> open_index(const std::string& dir, const source& src)
{
    index_files ix;
    auto t = mapped_vector<char>::open(at(dir, "text.bin"), {.type_tag = tag_of(src, "text"), .reserve = 0});
    if (t.is_err())
        return err(t.unwrap_err_unchecked());
    auto s = mapped_vector<std::int32_t>::open(at(dir, "sa.bin"), {.type_tag = tag_of(src, "sa"), .reserve = 0});
    if (s.is_err())
        return err(s.unwrap_err_unchecked());
    auto p = mapped_vector<std::uint32_t>::open(at(dir, "prefix.bin"), {.type_tag = tag_of(src, "prefix"), .reserve = 0});
    if (p.is_err())
        return err(p.unwrap_err_unchecked());
    if (s->size() != t->size() || p->size() != prefix_entries)
        return err(make_error_code(mapped_errc::file_too_small));
    ix.text = std::move(*t);
    ix.sa = std::move(*s);
    ix.prefix = std::move(*p);
    return ok(std::move(ix));
}

// The text file: generated (or copied from the source file) once
mapped_vector<char> make_text(const std::string& dir, const source& src)
{
    auto t = mapped_vector<char>::create(at(dir, "text.bin"), {.type_tag = tag_of(src, "text")});
    if (t.is_err())
        fail_hard("create text.bin", t.unwrap_err_unchecked());
    std::vector<char> chunk(1 << 20);
    if (src.file.empty()) {
        text_index::dna_generator g(src.seed);
        for (std::size_t done = 0; done < src.size;) {
            const std::size_t k = std::min(chunk.size(), src.size - done);
            g.fill(chunk.data(), k);
            if (auto r = t->append({chunk.data(), k}); r.is_err())
                fail_hard("write text.bin", r.unwrap_err_unchecked());
            done += k;
        }
    } else {
        std::FILE* f = std::fopen(src.file.c_str(), "rb");
        if (!f)
            fail_hard("open source file", {errno, std::system_category()});
        for (std::size_t k; (k = std::fread(chunk.data(), 1, chunk.size(), f)) > 0;) {
            for (std::size_t i = 0; i < k; ++i)   // 0 and 1 are reserved (the sentinel sorts below the text)
                if (static_cast<unsigned char>(chunk[i]) < 2)
                    chunk[i] = '\x02';
            if (auto r = t->append({chunk.data(), k}); r.is_err())
                fail_hard("write text.bin", r.unwrap_err_unchecked());
        }
        std::fclose(f);
    }
    if (auto r = t->push_back(sentinel); r.is_err())
        fail_hard("write text.bin", r.unwrap_err_unchecked());
    if (t->size() >= (std::size_t{1} << 31))
        fail_hard("text", make_error_code(mapped_errc::too_large));   // int32 positions
    if (auto r = t->flush(); r.is_err())
        fail_hard("flush text.bin", r.unwrap_err_unchecked());
    return std::move(*t);
}

template <class T>
mapped_vector<T> make_array(const std::string& path, std::uint64_t tag, std::size_t n)
{
    auto v = mapped_vector<T>::create(path, {.type_tag = tag, .initial_capacity = n});
    if (v.is_err())
        fail_hard(("create " + path).c_str(), v.unwrap_err_unchecked());
    if (auto r = v->resize(n); r.is_err())
        fail_hard(("size " + path).c_str(), r.unwrap_err_unchecked());
    if (auto r = v->flush(); r.is_err())   // the size is in the header from here, so a kill can find the file
        fail_hard(("flush " + path).c_str(), r.unwrap_err_unchecked());
    return std::move(*v);
}

template <class T>
mapped_vector<T> must_open(const std::string& path, std::uint64_t tag)
{
    auto v = mapped_vector<T>::open(path, {.type_tag = tag});
    if (v.is_err())
        fail_hard(("open " + path).c_str(), v.unwrap_err_unchecked());
    return std::move(*v);
}

template <class V>
void must_flush(V& v, const char* what)
{
    if (auto r = v.flush(); r.is_err())
        fail_hard(what, r.unwrap_err_unchecked());
}

struct build_report
{
    double ms = 0;
    std::size_t rounds = 0;
    std::size_t resumed_at_k = 0;   // 0: a fresh build
    bool built = false;             // false: the index was already there
};

// Open the index, building (or finishing) it if it is not complete. die_after_round > 0 kills the process after
// that many rounds of this run, for the resume test.
template <class Pool>
index_files open_or_build(Pool& pool, const std::string& dir, const source& src, build_report& rep,
                          bool durable, std::size_t die_after_round = 0)
{
    fs::create_directories(dir);
    if (auto ix = open_index(dir, src); ix.is_ok())
        return std::move(*ix);

    const auto t0 = clk::now();
    const bool trace = std::getenv("TSM_INDEX_TRACE") != nullptr;   // per-round timings
    rep.built = true;
    // The text, unless it is already there and of this source
    auto text_or = mapped_vector<char>::open(at(dir, "text.bin"), {.type_tag = tag_of(src, "text")});
    mapped_vector<char> text;
    if (text_or.is_ok()) {
        text = std::move(*text_or);
    } else {
        for (const char* f : {"sa.bin", "prefix.bin", "progress.bin", "rank0.bin", "rank1.bin"})
            fs::remove(at(dir, f));   // whatever was built belongs to other data
        text = make_text(dir, src);
    }
    const std::size_t n = text.size();
    const std::string_view s(text.data(), n);

    // Only the ranks are checkpointed. The order being sorted and the scan's scratch live in anonymous memory: a
    // resumed round needs neither (see suffix_array.hpp), and a parallel sort writing all over a FILE mapping fights
    // the kernel's writeback, since every page it cleans faults again on the next write (166 s against 1.2 s for 1M
    // bases on a Linux overlay filesystem). sa.bin is written once, at the end.
    auto workspace = [&](const char* what) {
        auto r = mapped_region::anonymous(n * sizeof(std::int32_t), n * sizeof(std::int32_t));
        if (r.is_err())
            fail_hard(what, r.unwrap_err_unchecked());
        return std::move(*r);
    };
    mapped_region sa_mem = workspace("sa workspace"), flags_mem = workspace("flags"), scanned_mem = workspace("scan");
    auto* const sa = reinterpret_cast<std::int32_t*>(sa_mem.data());
    auto* const flags = reinterpret_cast<std::int32_t*>(flags_mem.data());
    auto* const scanned = reinterpret_cast<std::int32_t*>(scanned_mem.data());

    const std::uint64_t work_tag = tag_of(src, "build");
    mapped_vector<std::int32_t> rank[2];
    mapped_vector<std::uint64_t> progress;   // [next k, which rank file is current]
    auto resumed = mapped_vector<std::uint64_t>::open(at(dir, "progress.bin"), {.type_tag = work_tag});
    if (resumed.is_ok() && resumed->size() == 2) {
        progress = std::move(*resumed);
        rank[0] = must_open<std::int32_t>(at(dir, "rank0.bin"), work_tag);
        rank[1] = must_open<std::int32_t>(at(dir, "rank1.bin"), work_tag);
        rep.resumed_at_k = progress[0];
        text_index::suffix_array_reset_order(pool, sa, n);   // any permutation will do; see suffix_array.hpp
    } else {
        rank[0] = make_array<std::int32_t>(at(dir, "rank0.bin"), work_tag, n);
        rank[1] = make_array<std::int32_t>(at(dir, "rank1.bin"), work_tag, n);
        text_index::suffix_array_init(pool, s, sa, rank[0].data());
        must_flush(rank[0], "flush rank0.bin");
        progress = make_array<std::uint64_t>(at(dir, "progress.bin"), work_tag, 2);
        progress[0] = 1;
        progress[1] = 0;
        must_flush(progress, "flush progress.bin");
    }

    for (std::size_t this_run = 1;; ++this_run) {
        const std::size_t k = progress[0];
        const std::size_t cur = progress[1];
        const auto r0 = clk::now();
        const bool done = text_index::suffix_array_round(pool, n, k, sa, rank[cur].data(), rank[1 - cur].data(),
                                                         flags, scanned);
        ++rep.rounds;
        const auto r1 = clk::now();
        // The next ranks are complete before progress says they exist. Against a killed process that order is
        // enough: every store is in the page cache, in program order. Against a power cut (--durable) each step
        // must also reach the disk, with an msync, before the next.
        if (durable)
            must_flush(rank[1 - cur], "flush rank");
        progress[0] = 2 * k;
        progress[1] = 1 - cur;
        if (durable)
            must_flush(progress, "flush progress.bin");
        if (trace)
            std::printf("      round k=%zu: sort and rank %.0f ms, checkpoint %.0f ms\n", k,
                        std::chrono::duration<double, std::milli>(r1 - r0).count(), ms_since(r1));
        if (die_after_round && this_run == die_after_round)
            ::kill(::getpid(), SIGKILL);
        if (done)
            break;
    }
    // The finished order, written to its file in one sequential pass
    auto sa_file = make_array<std::int32_t>(at(dir, "sa.bin"), tag_of(src, "sa"), n);
    std::memcpy(sa_file.data(), sa, n * sizeof(std::int32_t));
    must_flush(sa_file, "flush sa.bin");

    // The 2-byte prefix table: a count per prefix, then the running sum. A suffix of one byte (the sentinel) has a
    // missing second byte, which sorts first, as byte 0 would.
    auto prefix = make_array<std::uint32_t>(at(dir, "prefix.bin"), tag_of(src, "prefix"), prefix_entries);
    std::vector<std::uint32_t> count(65536, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned hi = static_cast<unsigned char>(s[i]);
        const unsigned lo = i + 1 < n ? static_cast<unsigned char>(s[i + 1]) : 0u;
        ++count[hi * 256 + lo];
    }
    std::uint32_t running = 0;
    for (std::size_t k = 0; k < 65536; ++k) {
        prefix[k] = running;
        running += count[k];
    }
    prefix[65536] = running;
    must_flush(prefix, "flush prefix.bin");   // the index is complete from here

    for (const char* f : {"progress.bin", "rank0.bin", "rank1.bin"})
        fs::remove(at(dir, f));
    rep.ms = ms_since(t0);

    index_files ix;
    ix.text = std::move(text);
    ix.sa = std::move(sa_file);
    ix.prefix = std::move(prefix);
    return ix;
}

// ------------------------------------------------------------------ searching

// Compare the suffix at p with the pattern, over the pattern's length: <0, 0 (the suffix starts with it), >0
int compare(std::string_view text, std::int32_t p, std::string_view pat)
{
    const std::string_view suffix = text.substr(static_cast<std::size_t>(p));
    const std::size_t m = std::min(suffix.size(), pat.size());
    const int c = std::memcmp(suffix.data(), pat.data(), m);
    if (c != 0)
        return c;
    return suffix.size() < pat.size() ? -1 : 0;
}

struct hits
{
    std::size_t first = 0, last = 0;   // the range of sa
    std::size_t count() const { return last - first; }
};

hits search(const index_files& ix, std::string_view pat)
{
    const std::string_view text = ix.view();
    std::size_t lo = 0, hi = ix.n();
    if (pat.size() >= 2) {
        const std::size_t key = static_cast<unsigned char>(pat[0]) * 256u + static_cast<unsigned char>(pat[1]);
        lo = ix.prefix[key];
        hi = ix.prefix[key + 1];
    } else if (pat.size() == 1) {
        const std::size_t key = static_cast<unsigned char>(pat[0]) * 256u;
        lo = ix.prefix[key];
        hi = ix.prefix[key + 256];
    }
    const std::int32_t* sa = ix.sa.data();
    const auto first = std::partition_point(sa + lo, sa + hi, [&](std::int32_t p) { return compare(text, p, pat) < 0; });
    const auto last = std::partition_point(first, sa + hi, [&](std::int32_t p) { return compare(text, p, pat) == 0; });
    return {static_cast<std::size_t>(first - sa), static_cast<std::size_t>(last - sa)};
}

std::size_t naive_count(std::string_view text, std::string_view pat)
{
    std::size_t c = 0;
    for (std::size_t at = text.find(pat); at != std::string_view::npos; at = text.find(pat, at + 1))
        ++c;
    return c;
}

// Patterns taken from the text (so they occur) and a few made up (which may not)
std::vector<std::string> patterns(std::string_view text, std::size_t count, unsigned seed)
{
    std::mt19937_64 rng(seed);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t len = 4 + rng() % 17;
        const std::size_t at = rng() % (text.size() - len - 1);
        out.emplace_back(text.substr(at, len));
    }
    out.emplace_back("GATTACAGATTACA");
    out.emplace_back("ACGTACGTTTTACGTACGTACGTTTTACGT");   // the motif, twice
    out.emplace_back("A");
    out.emplace_back("TT");
    return out;
}

// --------------------------------------------------------------- the checks

bool sa_is_valid(const index_files& ix)
{
    const std::size_t n = ix.n();
    std::vector<bool> seen(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        const auto p = static_cast<std::size_t>(ix.sa[i]);
        if (p >= n || seen[p])
            return false;
        seen[p] = true;
    }
    const std::string_view t = ix.view();
    for (std::size_t i = 1; i < n; ++i)
        if (!(t.substr(static_cast<std::size_t>(ix.sa[i - 1])) < t.substr(static_cast<std::size_t>(ix.sa[i]))))
            return false;
    return true;
}

void corrupt_byte(const std::string& file, long at)
{
    std::FILE* f = std::fopen(file.c_str(), "r+b");
    std::fseek(f, at, SEEK_SET);
    const int c = std::fgetc(f);
    std::fseek(f, at, SEEK_SET);
    std::fputc(c ^ 0x10, f);
    std::fclose(f);
}

std::string temp_dir(const char* name)
{
    const char* tmp = std::getenv("TMPDIR");
    return std::string(tmp && *tmp ? tmp : "/tmp") + "/tsm_text_index_" + std::to_string(::getpid()) + "_" + name;
}

} // namespace

int main(int argc, char** argv)
{
    source src;
    std::string dir;
    bool keep = false;
    bool durable = false;
    std::size_t threads = 0;
    std::vector<std::string> queries;
    bool query_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--size") src.size = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--seed") src.seed = static_cast<unsigned>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--file") src.file = next();
        else if (a == "--dir") dir = next();
        else if (a == "--keep") keep = true;
        else if (a == "--durable") durable = true;
        else if (a == "--threads") threads = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "query") query_mode = true;
        else if (query_mode) queries.push_back(a);
        else {
            std::fprintf(stderr, "usage: %s [--size N] [--seed S] [--file F] [--dir D] [--keep] [--durable] [--threads T] "
                                 "[query PATTERN...]\n", argv[0]);
            return 2;
        }
    }
    const bool own_dir = dir.empty();
    if (own_dir)
        dir = temp_dir("main");
    if (own_dir || query_mode)
        keep = keep || query_mode;

    work_stealing_task_pool pool(threads ? threads : std::max(1u, std::thread::hardware_concurrency()));

    // ----------------------------------------------------- query mode: build if needed, then search
    if (query_mode) {
        build_report rep;
        index_files ix = open_or_build(pool, dir, src, rep, durable);
        if (rep.built)
            std::printf("built the index of %zu bytes in %.0f ms (%zu rounds)\n", ix.n(), rep.ms, rep.rounds);
        for (const std::string& q : queries) {
            const auto t0 = clk::now();
            const hits h = search(ix, q);
            const double us = us_since(t0);
            std::printf("  %-32s %10zu hits  %8.1f us", q.c_str(), h.count(), us);
            for (std::size_t i = h.first; i < std::min(h.last, h.first + 5); ++i)
                std::printf("  @%d", ix.sa[i]);
            std::printf("\n");
        }
        return 0;
    }

    // ----------------------------------------------------------------------- the self-test
    std::printf("persistent full-text index: %s, %zu threads, page %zu, checkpoints %s\n",
                src.file.empty() ? ("synthetic DNA, " + std::to_string(src.size) + " bases").c_str() : src.file.c_str(),
                pool.worker_count(), mapped_region::page_size(),
                durable ? "synced to disk (--durable)" : "in the page cache (survive a kill)");
    fs::remove_all(dir);

    // 1. The first run builds
    std::printf("\n  1. the first run builds the index\n");
    build_report first;
    std::vector<std::string> pats;
    std::vector<std::size_t> expected;
    {
        index_files ix = open_or_build(pool, dir, src, first, durable);
        std::printf("    built: %zu bytes, %zu rounds, %.0f ms\n", ix.n(), first.rounds, first.ms);
        check(first.built && first.resumed_at_k == 0, "a fresh build");
        check(sa_is_valid(ix), "sa is a permutation, every adjacent pair of suffixes in order");
        pats = patterns(ix.view().substr(0, ix.n() - 1), 200, 7);
        bool all = true;
        for (const std::string& p : pats) {
            const std::size_t c = naive_count(ix.view(), p);
            expected.push_back(c);
            all = all && search(ix, p).count() == c;
        }
        check(all, "every search's count matches a naive scan of the text");
    }

    // 2. Every later run reopens it
    std::printf("\n  2. every later run reopens it instead\n");
    {
        const auto t0 = clk::now();
        build_report again;
        index_files ix = open_or_build(pool, dir, src, again, durable);
        const double open_us = us_since(t0);
        check(!again.built, "nothing is rebuilt");
        auto t1 = clk::now();
        const hits h = search(ix, pats[0]);
        const double first_us = us_since(t1);
        t1 = clk::now();
        std::size_t total = 0;
        bool all = true;
        for (int rep = 0; rep < 50; ++rep)
            for (std::size_t i = 0; i < pats.size(); ++i) {
                const std::size_t c = search(ix, pats[i]).count();
                total += c;
                all = all && c == expected[i];
            }
        const double warm_us = us_since(t1) / static_cast<double>(50 * pats.size());
        check(all && h.count() == expected[0], "the reopened index finds the same counts");
        std::printf("    build %.0f ms  ->  reopen %.0f us  (%.0fx); first search %.1f us, then %.2f us a search "
                    "(%zu patterns x 50, %zu hits)\n",
                    first.ms, open_us, first.ms * 1000.0 / open_us, first_us, warm_us, pats.size(), total);
    }

    // 3. A build killed part way resumes
    std::printf("\n  3. a build killed after round 3 resumes from its checkpoint\n");
    const std::string dir2 = temp_dir("resume");
    fs::remove_all(dir2);
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child == 0) {
        build_report r;
        (void)open_or_build(pool, dir2, src, r, durable, 3);
        ::_exit(9);   // not reached: the build kills itself
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "the child was killed in the middle of the build");
    {
        build_report resumed;
        index_files ix2 = open_or_build(pool, dir2, src, resumed, durable);
        std::printf("    resumed at k = %zu: %zu rounds left of %zu, %.0f ms\n", resumed.resumed_at_k, resumed.rounds,
                    first.rounds, resumed.ms);
        check(resumed.resumed_at_k == 8 && resumed.rounds + 3 == first.rounds, "it resumed after the third round");
        auto ix = open_index(dir, src);
        check(ix.is_ok() && std::memcmp(ix->sa.data(), ix2.sa.data(), ix2.n() * sizeof(std::int32_t)) == 0,
              "the resumed sa is byte for byte the uninterrupted one");
        check(!fs::exists(at(dir2, "progress.bin")) && !fs::exists(at(dir2, "rank0.bin")),
              "the build files are gone once the index is complete");
    }
    fs::remove_all(dir2);

    // 4. An index of other data, or a damaged one, is refused
    std::printf("\n  4. an index of other data, or a damaged one, is refused\n");
    {
        source other = src;
        other.seed += 1;
        check(open_index(dir, other).err() == mapped_errc::type_mismatch, "an index of different data: type_mismatch");
        corrupt_byte(at(dir, "sa.bin"), 40);   // inside the header's capacity
        check(open_index(dir, src).err() == mapped_errc::bad_checksum, "a corrupted header: bad_checksum");
        corrupt_byte(at(dir, "sa.bin"), 40);
        check(open_index(dir, src).is_ok(), "and once repaired it opens again");
    }

    // 5. Several processes search the same index
    std::printf("\n  5. four reader processes search the one index\n");
    std::fflush(stdout);
    std::vector<pid_t> readers;
    for (int r = 0; r < 4; ++r) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            const auto t0 = clk::now();
            auto ix = open_index(dir, src);
            if (ix.is_err())
                ::_exit(1);
            const double open_us = us_since(t0);
            const auto t1 = clk::now();
            for (std::size_t i = 0; i < pats.size(); ++i)
                if (search(*ix, pats[i]).count() != expected[i])
                    ::_exit(2);
            std::printf("    reader %d: opened in %.0f us, %zu searches in %.0f us\n", r, open_us, pats.size(),
                        us_since(t1));
            std::fflush(stdout);
            ::_exit(0);
        }
        readers.push_back(pid);
    }
    bool readers_ok = true;
    for (pid_t pid : readers) {
        int st = 0;
        ::waitpid(pid, &st, 0);
        readers_ok = readers_ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    check(readers_ok, "every reader found the same counts");

    if (!keep)
        fs::remove_all(dir);
    else
        std::printf("\n  the index is kept in %s\n", dir.c_str());
    std::printf("\n%s\n", failures == 0 ? "persistent_index_demo: every check passed" : "persistent_index_demo: FAILED");
    return failures == 0 ? 0 : 1;
}
