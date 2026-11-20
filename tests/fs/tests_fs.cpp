//
//  tests_fs.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - tests for fs/: read_file, read_file_bytes, write_file_atomically and full_sync.
//  Their own program (C++20 and POSIX, like mmap/), run by `make test-fs`:
//
//    - text, binary with NULs, empty, and 10 MB files read back exactly
//    - a missing file, a directory and an unreadable file are errors you can compare, not empty strings
//    - a FIFO, whose size the kernel does not know, is read whole
//    - a replace leaves no temporary behind, keeps the replaced file's mode, or takes the one asked for
//    - a failed write (missing directory, read-only directory) leaves the old file and no temporary
//    - readers racing writers only ever see one writer's complete contents; so do racing writers
//    - a child killed while replacing a file leaves the old contents or the new, never a mixture
//    - atomic_file_writer: 48 MiB streamed in uneven chunks with reserve(), the target unchanged until commit();
//      an abandoned writer leaves the target and no temporary; racing streamed writers leave one whole file;
//      a write that fails (RLIMIT_FSIZE: EFBIG) removes the temporary and keeps returning its error
//
//  Files go in $TMPDIR (or /tmp), in a directory of their own, removed at the end.
//

#include "../test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/file.hpp"

namespace {

namespace fs = std::filesystem;
using snicholls::atomic_file_writer;
using snicholls::read_file;
using snicholls::read_file_bytes;
using snicholls::write_file_atomically;

std::string dir;

std::string path(const std::string& name) { return dir + "/" + name; }

// Only the given names in the test's directory: no temporary left behind
std::size_t entries_in(const std::string& d)
{
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(d)) ++n;
    return n;
}

::mode_t mode_of(const std::string& p)
{
    struct stat st {};
    assert(::stat(p.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

bool running_as_root() { return ::geteuid() == 0; }

void test_round_trips()
{
    const std::string text = "line one\nline two\n";
    assert(write_file_atomically(path("text.txt"), text).is_ok());
    assert(*read_file(path("text.txt")) == text);

    std::string binary;
    for (int i = 0; i < 1024; ++i) binary.push_back(static_cast<char>(i % 256));   // NULs included
    assert(write_file_atomically(path("binary.bin"), binary).is_ok());
    assert(*read_file(path("binary.bin")) == binary);
    const auto bytes = read_file_bytes(path("binary.bin"));
    assert(bytes.is_ok() && bytes->size() == 1024 && (*bytes)[0] == 0 && (*bytes)[255] == 255 && (*bytes)[256] == 0);

    std::vector<std::uint8_t> payload(10u << 20);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 2654435761u >> 24);
    assert(write_file_atomically(path("large.bin"), payload).is_ok());
    assert(*read_file_bytes(path("large.bin")) == payload);

    assert(write_file_atomically(path("empty.txt"), std::string_view{}).is_ok());
    const auto empty = read_file(path("empty.txt"));
    assert(empty.is_ok() && empty->empty());
    pass("read_file: text, binary with NULs, 10 MB and empty files round-trip exactly");
}

void test_read_errors_are_distinguishable()
{
    const auto missing = read_file(path("no-such-file"));
    assert(missing.is_err() && missing.unwrap_err_unchecked() == std::errc::no_such_file_or_directory);

    const auto directory = read_file(dir);
    assert(directory.is_err() && directory.unwrap_err_unchecked() == std::errc::is_a_directory);

    if (!running_as_root()) {
        assert(write_file_atomically(path("secret.txt"), "x", {.mode = fs::perms::none}).is_ok());
        const auto denied = read_file(path("secret.txt"));
        assert(denied.is_err() && denied.unwrap_err_unchecked() == std::errc::permission_denied);
    }
    pass("read_file: missing, a directory and unreadable are errors you can compare, not empty strings");
}

void test_reads_a_file_of_unknown_size()
{
    const std::string fifo = path("pipe");
    assert(::mkfifo(fifo.c_str(), 0600) == 0);
    std::string expected;
    for (int i = 0; i < 20000; ++i) expected += "chunk " + std::to_string(i) + "\n";   // past any pipe buffer
    std::thread writer([&] {
        const int fd = ::open(fifo.c_str(), O_WRONLY);
        assert(fd >= 0);
        std::size_t done = 0;
        while (done < expected.size()) {
            const auto n = ::write(fd, expected.data() + done, std::min<std::size_t>(777, expected.size() - done));
            assert(n > 0);
            done += static_cast<std::size_t>(n);
        }
        ::close(fd);
    });
    const auto got = read_file(fifo);
    writer.join();
    assert(got.is_ok() && *got == expected);
    pass("read_file: a FIFO, whose size the kernel does not know, is read whole");
}

void test_replace_keeps_mode_and_leaves_nothing_behind()
{
    const std::string d = path("modes");
    fs::create_directory(d);
    const std::string p = d + "/config";

    assert(write_file_atomically(p, "first", {.mode = fs::perms::owner_read | fs::perms::owner_write}).is_ok());
    assert(mode_of(p) == 0600);
    assert(write_file_atomically(p, "second").is_ok());
    assert(*read_file(p) == "second");
    assert(mode_of(p) == 0600);   // the replaced file's mode carried over
    assert(write_file_atomically(p, "third", {.mode = fs::perms::owner_all | fs::perms::group_read}).is_ok());
    assert(mode_of(p) == 0740);
    assert(entries_in(d) == 1);

    assert(write_file_atomically(p, "fast", {.durable = false}).is_ok());
    assert(*read_file(p) == "fast");
    assert(entries_in(d) == 1);
    pass("write_file_atomically: replaces, keeps the old mode or takes the one asked for, leaves no temporary");
}

void test_failed_writes_change_nothing()
{
    const auto nowhere = write_file_atomically(path("missing-dir/file"), "x");
    assert(nowhere.is_err() && nowhere.unwrap_err_unchecked() == std::errc::no_such_file_or_directory);
    assert(!fs::exists(path("missing-dir")));

    assert(write_file_atomically(path("made/on/demand/file"), "x", {.create_directories = true}).is_ok());
    assert(*read_file(path("made/on/demand/file")) == "x");

    const auto into_a_directory = write_file_atomically(dir + "/", "x");
    assert(into_a_directory.is_err());

    if (!running_as_root()) {
        const std::string d = path("locked");
        fs::create_directory(d);
        assert(write_file_atomically(d + "/file", "old").is_ok());
        fs::permissions(d, fs::perms::owner_read | fs::perms::owner_exec);
        const auto denied = write_file_atomically(d + "/file", "new");
        fs::permissions(d, fs::perms::owner_all);
        assert(denied.is_err() && denied.unwrap_err_unchecked() == std::errc::permission_denied);
        assert(*read_file(d + "/file") == "old");
        assert(entries_in(d) == 1);
    }
    pass("write_file_atomically: a failed write leaves the old file and no temporary");
}

void test_readers_and_writers_race()
{
    // Each writer's contents are one repeated letter at a length of its own, so a mixture is easy to see
    const std::string p = path("raced");
    const std::size_t before = entries_in(dir);
    assert(write_file_atomically(p, std::string(5000, 'a'), {.durable = false}).is_ok());
    auto whole = [](const std::string& s) {
        if (s.empty()) return false;
        const std::size_t expect = 5000 + static_cast<std::size_t>(s[0] - 'a') * 3001;
        return s.size() == expect && s.find_first_not_of(s[0]) == std::string::npos;
    };

    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < 4; ++w) {
        threads.emplace_back([&, w] {
            for (int i = 0; i < 200; ++i) {
                const char c = static_cast<char>('a' + (w * 200 + i) % 26);
                const std::string contents(5000 + static_cast<std::size_t>(c - 'a') * 3001, c);
                assert(write_file_atomically(p, contents, {.durable = false}).is_ok());
            }
        });
    }
    for (int r = 0; r < 3; ++r) {
        threads.emplace_back([&] {
            while (!stop.load()) {
                const auto got = read_file(p);
                assert(got.is_ok() && whole(*got));
                ++reads;
            }
        });
    }
    for (int w = 0; w < 4; ++w) threads[static_cast<std::size_t>(w)].join();
    stop = true;
    for (std::size_t t = 4; t < threads.size(); ++t) threads[t].join();
    assert(whole(*read_file(p)));
    assert(entries_in(dir) == before + 1);   // only `raced` was added: every temporary was renamed or removed
    std::cout << "      (" << reads.load() << " reads, each one writer's whole file)\n";
    pass("write_file_atomically: racing readers and writers only ever see one writer's complete contents");
}

void test_a_killed_writer_leaves_old_or_new()
{
    const std::string p = path("killed");
    const std::string old_contents(1u << 20, 'o');
    const std::string new_contents(1u << 20, 'n');
    for (int round = 0; round < 20; ++round) {
        assert(write_file_atomically(p, old_contents, {.durable = false}).is_ok());
        const pid_t child = ::fork();
        assert(child >= 0);
        if (child == 0) {
            for (;;) {
                (void)write_file_atomically(p, new_contents, {.durable = false});
                (void)write_file_atomically(p, old_contents, {.durable = false});
            }
        }
        ::usleep(static_cast<useconds_t>(1000 + round * 700));
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        const auto got = read_file(p);
        assert(got.is_ok() && (*got == old_contents || *got == new_contents));
    }
    // A killed writer can leave its temporary (nothing runs to remove it); never a damaged `killed`
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().filename().string().rfind(".killed.tmp.", 0) == 0) fs::remove(e.path());
    pass("write_file_atomically: a writer killed mid-replace leaves the old contents or the new, never a mixture");
}

void test_full_sync()
{
    const std::string p = path("synced");
    assert(write_file_atomically(p, "durable").is_ok());
    const int fd = ::open(p.c_str(), O_RDONLY);
    assert(fd >= 0);
    assert(snicholls::full_sync(fd).is_ok());
    ::close(fd);
    const auto bad = snicholls::full_sync(-1);
    assert(bad.is_err() && bad.unwrap_err_unchecked() == std::errc::bad_file_descriptor);
    pass("full_sync: a file syncs; a bad descriptor is EBADF");
}

// Byte i of a streamed file: a pattern no chunking mistake (a dropped, repeated or reordered chunk) survives
std::uint8_t pattern_byte(std::uint64_t i, std::uint8_t seed)
{
    return static_cast<std::uint8_t>((i * 2654435761u >> 13) ^ (i >> 20) ^ seed);
}

// Stream `total` bytes through `w` in chunks of uneven sizes, from 1 byte to 1 MiB
void stream_pattern(atomic_file_writer& w, std::uint64_t total, std::uint8_t seed)
{
    static const std::size_t sizes[] = {1, 7, 4093, 65535, 65536, 65537, 100000, 1u << 20};
    std::vector<std::uint8_t> chunk;
    std::uint64_t at = 0;
    for (std::size_t k = 0; at < total; ++k) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(sizes[k % 8], total - at));
        chunk.resize(n);
        for (std::size_t j = 0; j < n; ++j) chunk[j] = pattern_byte(at + j, seed);
        assert(w.append(std::span<const std::uint8_t>(chunk)).is_ok());
        at += n;
    }
    assert(w.size() == total);
}

bool holds_pattern(const std::vector<std::uint8_t>& got, std::uint64_t total, std::uint8_t seed)
{
    if (got.size() != total) return false;
    for (std::uint64_t i = 0; i < total; ++i)
        if (got[static_cast<std::size_t>(i)] != pattern_byte(i, seed)) return false;
    return true;
}

void test_writer_streams_a_large_file()
{
    const std::string d = path("streamed");
    fs::create_directory(d);
    const std::string p = d + "/model.bin";
    assert(write_file_atomically(p, "old", {.mode = fs::perms::owner_read | fs::perms::owner_write}).is_ok());

    const std::uint64_t total = 48ull << 20;
    auto w = atomic_file_writer::create(p);
    assert(w.is_ok() && w->active() && w->target() == p && !w->temporary().empty());
    assert(w->temporary().parent_path() == fs::path(d));                // beside the target: the rename is one step
    assert(w->reserve(total).is_ok());
    stream_pattern(*w, total, 0x5a);
    assert(w->append(std::string_view{}).is_ok());                       // an empty append is a no-op
    assert(*read_file(p) == "old");                                       // nothing visible before commit()
    assert(entries_in(d) == 2);                                           // the target and the temporary
    const fs::path tmp = w->temporary();
    assert(w->commit().is_ok());
    assert(!w->active() && w->temporary().empty() && !fs::exists(tmp));
    assert(entries_in(d) == 1);
    assert(holds_pattern(*read_file_bytes(p), total, 0x5a));             // and the reserve() left no tail
    assert(mode_of(p) == 0600);                                           // the replaced file's mode carried over

    // Finished: every later call says so, and the file stays
    const auto again = w->append(std::string_view("more"));
    assert(again.is_err() && again.unwrap_err_unchecked() == std::errc::bad_file_descriptor);
    assert(w->commit().is_err());
    assert(fs::file_size(p) == total);

    // std::byte spans, a mode asked for, directories made on demand, not durable
    const std::string deep = path("streamed/a/b/c.bin");
    auto v = atomic_file_writer::create(deep, {.durable = false, .mode = fs::perms::owner_all,
                                               .create_directories = true});
    assert(v.is_ok());
    const std::byte two[] = {std::byte{1}, std::byte{2}};
    assert(v->append(std::span<const std::byte>(two)).is_ok() && v->commit().is_ok());
    assert(*read_file(deep) == std::string("\x01\x02", 2) && mode_of(deep) == 0700);
    std::cout << "      (" << (total >> 20) << " MiB in " << 8 << " chunk sizes from 1 byte to 1 MiB)\n";
    pass("atomic_file_writer: a large file streamed in uneven chunks replaces the target only at commit()");
}

void test_writer_abandoned_leaves_nothing()
{
    const std::string d = path("abandoned");
    fs::create_directory(d);
    const std::string p = d + "/target";
    assert(write_file_atomically(p, "untouched").is_ok());
    {
        auto w = atomic_file_writer::create(p);
        assert(w.is_ok());
        stream_pattern(*w, 3u << 20, 1);
        assert(entries_in(d) == 2);
    }   // destroyed without commit()
    assert(*read_file(p) == "untouched" && entries_in(d) == 1);

    auto w = atomic_file_writer::create(p);
    assert(w.is_ok() && w->append(std::string_view("discarded")).is_ok());
    atomic_file_writer moved = std::move(*w);                             // the temporary goes with the move
    assert(!w->active() && moved.active() && entries_in(d) == 2);
    moved.abandon();
    assert(!moved.active() && entries_in(d) == 1 && *read_file(p) == "untouched");
    assert(moved.commit().is_err() && *read_file(p) == "untouched");

    // A writer for a file that does not exist yet, abandoned: still nothing
    {
        auto n = atomic_file_writer::create(d + "/new");
        assert(n.is_ok() && n->append(std::string_view("x")).is_ok());
    }
    assert(!fs::exists(d + "/new") && entries_in(d) == 1);

    const auto nowhere = atomic_file_writer::create(path("no-such-dir/file"));
    assert(nowhere.is_err() && nowhere.unwrap_err_unchecked() == std::errc::no_such_file_or_directory);
    assert(atomic_file_writer::create(dir + "/").is_err());
    pass("atomic_file_writer: destroyed or abandoned before commit(), the target is untouched and no temporary remains");
}

void test_writers_race_to_one_target()
{
    const std::string d = path("racing-writers");
    fs::create_directory(d);
    const std::string p = d + "/target";
    constexpr int writers = 6;
    constexpr std::uint64_t total = 2u << 20;

    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    auto whole = [&](const std::vector<std::uint8_t>& got) {
        for (int k = 0; k < writers; ++k)
            if (holds_pattern(got, total, static_cast<std::uint8_t>(k))) return true;
        return false;
    };
    {
        auto first = atomic_file_writer::create(p, {.durable = false});   // so readers have a file from the start
        assert(first.is_ok());
        stream_pattern(*first, total, 0);
        assert(first->commit().is_ok());
    }
    std::vector<std::thread> threads;
    for (int k = 0; k < writers; ++k) {
        threads.emplace_back([&, k] {
            for (int round = 0; round < 4; ++round) {
                auto w = atomic_file_writer::create(p, {.durable = false});
                assert(w.is_ok());
                stream_pattern(*w, total, static_cast<std::uint8_t>(k));
                assert(w->commit().is_ok());
            }
        });
    }
    threads.emplace_back([&] {
        while (!stop.load()) {
            const auto got = read_file_bytes(p);
            assert(got.is_ok() && whole(*got));
            ++reads;
        }
    });
    for (int k = 0; k < writers; ++k) threads[static_cast<std::size_t>(k)].join();
    stop = true;
    threads.back().join();
    assert(whole(*read_file_bytes(p)));
    assert(entries_in(d) == 1);
    std::cout << "      (" << writers * 4 << " streamed commits, " << reads.load() << " reads, each one whole file)\n";
    pass("atomic_file_writer: racing writers to one target leave one writer's whole file and no temporaries");
}

void test_writer_error_removes_the_temporary()
{
    // RLIMIT_FSIZE turns a write past 1 MiB into EFBIG (with SIGXFSZ ignored): a real write error, even as root
    const std::string d = path("write-error");
    fs::create_directory(d);
    const std::string p = d + "/target";
    assert(write_file_atomically(p, "before").is_ok());

    struct rlimit saved {};
    assert(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
    auto* const old_handler = ::signal(SIGXFSZ, SIG_IGN);
    struct rlimit small = saved;
    small.rlim_cur = 1u << 20;
    assert(::setrlimit(RLIMIT_FSIZE, &small) == 0);

    auto w = atomic_file_writer::create(p);
    assert(w.is_ok());
    const std::string chunk(256u << 10, 'z');
    std::error_code first;
    for (int i = 0; i < 16 && !first; ++i)
        if (auto a = w->append(chunk); a.is_err()) first = a.unwrap_err_unchecked();
    const bool removed_at_once = entries_in(d) == 1;
    const auto later = w->append(std::string_view("x"));
    const auto commit = w->commit();

    assert(::setrlimit(RLIMIT_FSIZE, &saved) == 0);
    ::signal(SIGXFSZ, old_handler);

    assert(first == std::errc::file_too_large);
    assert(removed_at_once && !w->active());
    assert(later.is_err() && later.unwrap_err_unchecked() == first);      // the first error, again
    assert(commit.is_err() && commit.unwrap_err_unchecked() == first);
    assert(*read_file(p) == "before" && entries_in(d) == 1);

    // write_file_atomically fails the same way
    assert(::setrlimit(RLIMIT_FSIZE, &small) == 0);
    ::signal(SIGXFSZ, SIG_IGN);
    const auto whole = write_file_atomically(p, std::string(2u << 20, 'y'));
    assert(::setrlimit(RLIMIT_FSIZE, &saved) == 0);
    ::signal(SIGXFSZ, old_handler);
    assert(whole.is_err() && whole.unwrap_err_unchecked() == std::errc::file_too_large);
    assert(*read_file(p) == "before" && entries_in(d) == 1);
    pass("atomic_file_writer: a failed write (EFBIG) removes the temporary, keeps the target, and repeats its error");
}

} // namespace

int main()
{
    const char* tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/tsm_fs_XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        std::perror("mkdtemp");
        return 1;
    }
    dir = buf.data();

    test_round_trips();
    test_read_errors_are_distinguishable();
    test_reads_a_file_of_unknown_size();
    test_replace_keeps_mode_and_leaves_nothing_behind();
    test_failed_writes_change_nothing();
    test_readers_and_writers_race();
    test_a_killed_writer_leaves_old_or_new();
    test_full_sync();
    test_writer_streams_a_large_file();
    test_writer_abandoned_leaves_nothing();
    test_writers_race_to_one_target();
    test_writer_error_removes_the_temporary();

    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    std::filesystem::remove_all(dir);
    std::cout << "\nfs: all " << tests_run << " tests passed\n";
    return 0;
}
