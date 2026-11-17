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
//
//  Files go in $TMPDIR (or /tmp), in a directory of their own, removed at the end.
//

#include "../test_helpers.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/file.hpp"

namespace {

namespace fs = std::filesystem;
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

    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    std::filesystem::remove_all(dir);
    std::cout << "\nfs: all " << tests_run << " tests passed\n";
    return 0;
}
