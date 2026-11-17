//
//  file.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - read a whole file, and replace a file so that a
//  reader or a crash only ever sees the old contents or the new ones:
//
//      auto text = snicholls::read_file("book.csv");          // result<std::string, std::error_code>
//      if (text.is_err()) return text.unwrap_err_unchecked();  // e.g. no_such_file_or_directory
//
//      auto w = snicholls::write_file_atomically("book.csv", updated);
//      // durable by default: the data, then the rename, are on stable storage when it returns
//
//  ---------------------------------------------------------------- read_file
//
//  read_file(path) returns the bytes as a std::string, read_file_bytes(path)
//  as a std::vector<std::uint8_t>. Both open the file once, size the buffer
//  from fstat, and read until end of file, so a file whose size the kernel
//  does not know (/proc, a FIFO, a character device) is read whole too.
//  EINTR is retried. Every failure is a std::error_code you can compare:
//  std::errc::no_such_file_or_directory for a missing file,
//  is_a_directory for a directory, permission_denied, and so on.
//
//  ----------------------------------------------------- write_file_atomically
//
//  write_file_atomically(path, bytes, options) writes `bytes` to a temporary
//  file beside `path` (same directory, so the rename cannot cross a
//  filesystem), then renames it over `path`. rename() replaces a name in one
//  step, so every reader opens either the whole old file or the whole new
//  one, never a mixture, and concurrent writers leave exactly one writer's
//  complete contents. With `durable` (the default) the temporary is synced
//  (full_sync: F_FULLFSYNC on macOS) before the rename and the directory
//  after it, so after a power cut the file holds the old contents or the new
//  - without the first sync a crash can leave the new name on an empty file.
//
//  The temporary is created with O_EXCL and O_NOFOLLOW under a name no other
//  writer in this process or another can be using (`.<name>.tmp.<pid>.<n>`),
//  and it is removed on every failure. The new file takes `mode` if given,
//  else the mode of the file it replaces, else 0666 less the umask. Owner and
//  extended attributes are not carried over. If `path` is a symbolic link,
//  the link itself is replaced, not its target.
//
//  All of these come from comparing the copies in use: none synced the
//  directory or retried EINTR, most could not tell a missing file from an
//  empty one, and fixed temporary names let two writers corrupt each other.
//
//  C++20 and POSIX, like mmap/.
//

#ifndef ts_moveables_fs_file_hpp
#define ts_moveables_fs_file_hpp

#if !defined(_WIN32)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <snicholls/core/result.hpp>

#include "sync.hpp"

namespace snicholls {

struct write_file_options
{
    bool durable = true;                             // full_sync the file before the rename and the directory after
    std::optional<std::filesystem::perms> mode{};    // the new file's mode; default: the replaced file's, else 0666 & ~umask
    bool create_directories = false;                 // create missing parent directories first
};

namespace detail::fs_file {

class fd_guard
{
public:
    explicit fd_guard(int fd) noexcept : fd_(fd) {}
    fd_guard(const fd_guard&) = delete;
    fd_guard& operator=(const fd_guard&) = delete;
    ~fd_guard() { if (fd_ >= 0) ::close(fd_); }
    int get() const noexcept { return fd_; }
    // Close now, reporting the error close() can give (EIO on some filesystems)
    std::error_code close() noexcept
    {
        const int fd = fd_;
        fd_ = -1;
        return ::close(fd) == 0 || errno == EINTR ? std::error_code{} : errno_code();
    }

private:
    int fd_;
};

inline int open_retrying(const char* path, int flags, ::mode_t mode = 0) noexcept
{
    for (;;) {
        const int fd = ::open(path, flags, mode);
        if (fd >= 0 || errno != EINTR) return fd;
    }
}

template <class Buffer>
result<Buffer, std::error_code> read_whole(const std::filesystem::path& path)
{
    fd_guard fd(open_retrying(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) return err(errno_code());
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) return err(errno_code());
    if (S_ISDIR(st.st_mode)) return err(std::make_error_code(std::errc::is_a_directory));

    Buffer out;
    // A regular file says how big it is; read one byte past that, so a file that grew meanwhile is still read whole
    std::size_t capacity = 4096;
    if (S_ISREG(st.st_mode) && st.st_size > 0) {
        if (static_cast<std::uintmax_t>(st.st_size) >= out.max_size())
            return err(std::make_error_code(std::errc::file_too_large));
        capacity = static_cast<std::size_t>(st.st_size) + 1;
    }
    std::size_t used = 0;
    out.resize(capacity);
    for (;;) {
        if (used == out.size()) {
            if (out.size() > out.max_size() / 2) return err(std::make_error_code(std::errc::file_too_large));
            out.resize(out.size() * 2);
        }
        const ::ssize_t n = ::read(fd.get(), out.data() + used, out.size() - used);
        if (n > 0) { used += static_cast<std::size_t>(n); continue; }
        if (n == 0) break;
        if (errno != EINTR) return err(errno_code());
    }
    out.resize(used);
    return ok(std::move(out));
}

inline std::filesystem::path directory_of(const std::filesystem::path& path)
{
    std::filesystem::path dir = path.parent_path();
    return dir.empty() ? std::filesystem::path(".") : dir;
}

inline std::filesystem::path temporary_beside(const std::filesystem::path& path)
{
    static std::atomic<std::uint64_t> counter{0};
    std::string name = ".";
    name += path.filename().string();
    name += ".tmp.";
    name += std::to_string(::getpid());
    name += '.';
    name += std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return directory_of(path) / name;
}

inline result<void, std::error_code> write_all(int fd, const unsigned char* data, std::size_t size) noexcept
{
    while (size > 0) {
        const std::size_t chunk = std::min<std::size_t>(size, std::size_t{1} << 30);
        const ::ssize_t n = ::write(fd, data, chunk);
        if (n > 0) {
            data += n;
            size -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return err(n < 0 ? errno_code() : std::make_error_code(std::errc::io_error));
    }
    return ok();
}

inline result<void, std::error_code> write_atomically(const std::filesystem::path& path, const unsigned char* data,
                                                      std::size_t size, const write_file_options& opt)
{
    if (path.filename().empty()) return err(std::make_error_code(std::errc::is_a_directory));
    const std::filesystem::path dir = directory_of(path);
    if (opt.create_directories) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return err(ec);
    }

    // The mode: asked for, else the replaced file's, else what open() gives under the umask
    std::optional<::mode_t> mode;
    if (opt.mode) {
        mode = static_cast<::mode_t>(*opt.mode) & 07777;
    } else if (struct stat st {}; ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        mode = st.st_mode & 07777;
    }

    // A fresh name; O_EXCL means a clash with a leftover is retried rather than written through
    std::filesystem::path tmp;
    int raw = -1;
    for (int attempt = 0; attempt < 16 && raw < 0; ++attempt) {
        tmp = temporary_beside(path);
        raw = open_retrying(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (raw < 0 && errno != EEXIST) return err(errno_code());
    }
    if (raw < 0) return err(std::make_error_code(std::errc::file_exists));
    fd_guard fd(raw);

    auto fail = [&](std::error_code e) -> result<void, std::error_code> {
        ::unlink(tmp.c_str());
        return err(e);
    };
    if (mode && ::fchmod(fd.get(), *mode) != 0) return fail(errno_code());
    if (auto w = write_all(fd.get(), data, size); w.is_err()) return fail(w.unwrap_err_unchecked());
    if (opt.durable) {
        if (auto s = full_sync(fd.get()); s.is_err()) return fail(s.unwrap_err_unchecked());
    }
    if (auto e = fd.close()) return fail(e);
    if (::rename(tmp.c_str(), path.c_str()) != 0) return fail(errno_code());

    if (opt.durable) {
        // The rename is a change to the directory: sync it, or a crash can bring back the old name
        fd_guard d(open_retrying(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (d.get() < 0) return err(errno_code());
        if (auto s = full_sync(d.get()); s.is_err()) return s;
    }
    return ok();
}

} // namespace detail::fs_file

// The whole file as text (any bytes, NULs included)
[[nodiscard]] inline result<std::string, std::error_code> read_file(const std::filesystem::path& path)
{
    return detail::fs_file::read_whole<std::string>(path);
}

// The whole file as bytes
[[nodiscard]] inline result<std::vector<std::uint8_t>, std::error_code> read_file_bytes(
    const std::filesystem::path& path)
{
    return detail::fs_file::read_whole<std::vector<std::uint8_t>>(path);
}

// Replace `path` with `bytes`: a reader or a crash sees the old contents or the new, never part of either
[[nodiscard]] inline result<void, std::error_code> write_file_atomically(const std::filesystem::path& path,
                                                                        std::string_view bytes,
                                                                        const write_file_options& opt = {})
{
    return detail::fs_file::write_atomically(path, reinterpret_cast<const unsigned char*>(bytes.data()),
                                             bytes.size(), opt);
}

[[nodiscard]] inline result<void, std::error_code> write_file_atomically(const std::filesystem::path& path,
                                                                        std::span<const std::uint8_t> bytes,
                                                                        const write_file_options& opt = {})
{
    return detail::fs_file::write_atomically(path, bytes.data(), bytes.size(), opt);
}

} // namespace snicholls

#endif // !_WIN32

#endif // ts_moveables_fs_file_hpp
