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
//      auto out = snicholls::atomic_file_writer::create("model.bin");   // too big to hold in memory
//      for (auto chunk : chunks) if (auto a = out->append(chunk); a.is_err()) return a;
//      if (auto c = out->commit(); c.is_err()) return c;                  // or destroy it: the target is untouched
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
//  ------------------------------------------------------ atomic_file_writer
//
//  The same replacement, streamed: atomic_file_writer::create(path, options)
//  makes the temporary by the same rules (fresh name, O_EXCL | O_NOFOLLOW,
//  the mode as above, create_directories honoured), append() adds bytes
//  (small appends are gathered into 64 KiB writes, large ones go straight
//  through), reserve(n) allocates blocks for n bytes up front so a full disk
//  is an error before the data is produced, and commit() syncs the file,
//  renames it over the target and syncs the directory, as write_file_atomically
//  does (which is now create, one append, commit). A writer destroyed or
//  abandon()ed before commit() removes its temporary; the target is never
//  touched until the rename. The first error removes the temporary at once and
//  is returned again by every later call. One writer, one thread; any number
//  of writers may race to the same target, and one of them wins whole.
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
#include <utility>
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

} // namespace detail::fs_file

// A file written piece by piece and then put in place atomically, like
// write_file_atomically but without the whole contents in memory: create()
// makes the temporary beside the target, append() adds bytes, and commit()
// replaces the target. A writer destroyed (or abandon()ed) before commit()
// removes its temporary and leaves the target as it was.
class atomic_file_writer
{
public:
    atomic_file_writer() noexcept = default;
    atomic_file_writer(const atomic_file_writer&) = delete;
    atomic_file_writer& operator=(const atomic_file_writer&) = delete;
    atomic_file_writer(atomic_file_writer&& o) noexcept { take(o); }
    atomic_file_writer& operator=(atomic_file_writer&& o) noexcept
    {
        if (this != &o) {
            abandon();
            take(o);
        }
        return *this;
    }
    ~atomic_file_writer() { abandon(); }

    // Create the temporary beside `path`: a fresh name, O_EXCL | O_NOFOLLOW, with the mode the new file will have
    [[nodiscard]] static result<atomic_file_writer, std::error_code> create(const std::filesystem::path& path,
                                                                            const write_file_options& opt = {})
    {
        using namespace detail::fs_file;
        if (path.filename().empty()) return err(std::make_error_code(std::errc::is_a_directory));
        if (opt.create_directories) {
            std::error_code ec;
            std::filesystem::create_directories(directory_of(path), ec);
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
        atomic_file_writer w;
        for (int attempt = 0; attempt < 16 && w.fd_ < 0; ++attempt) {
            w.tmp_ = temporary_beside(path);
            w.fd_ = open_retrying(w.tmp_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
            if (w.fd_ < 0 && errno != EEXIST) return err(errno_code());
        }
        if (w.fd_ < 0) return err(std::make_error_code(std::errc::file_exists));
        w.target_ = path;
        w.durable_ = opt.durable;
        if (mode && ::fchmod(w.fd_, *mode) != 0) return err(w.fail(errno_code()));
        return ok(std::move(w));
    }

    // Add bytes at the end. The first error is kept: the temporary is removed, and every later call returns it.
    [[nodiscard]] result<void, std::error_code> append(std::span<const std::uint8_t> bytes)
    {
        if (fd_ < 0) return err(stopped());
        if (bytes.size() >= buffer_bytes - buffer_.size()) {
            if (auto e = drain()) return err(e);
        }
        if (bytes.size() >= buffer_bytes) {
            if (auto w = detail::fs_file::write_all(fd_, bytes.data(), bytes.size()); w.is_err())
                return err(fail(w.unwrap_err_unchecked()));
        } else {
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
        }
        written_ += bytes.size();
        return ok();
    }
    [[nodiscard]] result<void, std::error_code> append(std::span<const std::byte> bytes)
    {
        return append(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    }
    [[nodiscard]] result<void, std::error_code> append(std::string_view bytes)
    {
        return append(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    }

    // Allocate blocks for `total_bytes` now, so a full disk is an error here rather than part way through.
    // Best effort where the filesystem cannot preallocate; the file's length stays what was appended.
    [[nodiscard]] result<void, std::error_code> reserve(std::uint64_t total_bytes)
    {
        if (fd_ < 0) return err(stopped());
        if (total_bytes <= reserved_) return ok();
        if (auto e = drain()) return err(e);
#if defined(__APPLE__)
        if (total_bytes <= written_) return ok();
        fstore_t fs{};
        fs.fst_flags = F_ALLOCATEALL;
        fs.fst_posmode = F_PEOFPOSMODE;   // from the end of what is written
        fs.fst_length = static_cast<off_t>(total_bytes - written_);
        if (::fcntl(fd_, F_PREALLOCATE, &fs) == -1 && errno != ENOTSUP && errno != EINVAL)
            return err(fail(errno_code()));
#else
        const int rc = ::posix_fallocate(fd_, 0, static_cast<off_t>(total_bytes));
        if (rc != 0 && rc != EOPNOTSUPP && rc != EINVAL) return err(fail({rc, std::system_category()}));
#endif
        reserved_ = total_bytes;
        return ok();
    }

    // Put the file in place: sync it (if durable), rename it over the target, sync the directory (if durable).
    // On failure before the rename the temporary is removed and the target is as it was. Ends the writer.
    [[nodiscard]] result<void, std::error_code> commit()
    {
        using namespace detail::fs_file;
        if (fd_ < 0) return err(stopped());
        if (auto e = drain()) return err(e);
        // posix_fallocate may have lengthened the file past what was written
        if (reserved_ > written_ && ::ftruncate(fd_, static_cast<off_t>(written_)) != 0) return err(fail(errno_code()));
        if (durable_) {
            if (auto r = full_sync(fd_); r.is_err()) return err(fail(r.unwrap_err_unchecked()));
        }
        const int fd = std::exchange(fd_, -1);
        if (::close(fd) != 0 && errno != EINTR) return err(fail(errno_code()));
        if (::rename(tmp_.c_str(), target_.c_str()) != 0) return err(fail(errno_code()));
        tmp_.clear();
        if (durable_) {
            // The rename is a change to the directory: sync it, or a crash can bring back the old name
            fd_guard d(open_retrying(directory_of(target_).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (d.get() < 0) return err(errno_code());
            if (auto r = full_sync(d.get()); r.is_err()) return r;
        }
        return ok();
    }

    // Remove the temporary now; the target is untouched. Safe to call at any time.
    void abandon() noexcept
    {
        if (fd_ >= 0) ::close(std::exchange(fd_, -1));
        if (!tmp_.empty()) {
            ::unlink(tmp_.c_str());
            tmp_.clear();
        }
    }

    bool active() const noexcept { return fd_ >= 0; }                            // created, not committed, no error
    std::uint64_t size() const noexcept { return written_; }                     // bytes appended so far
    const std::filesystem::path& target() const noexcept { return target_; }
    const std::filesystem::path& temporary() const noexcept { return tmp_; }     // empty once committed or abandoned

private:
    static constexpr std::size_t buffer_bytes = std::size_t{1} << 16;

    std::error_code stopped() const noexcept
    {
        return error_ ? error_ : std::make_error_code(std::errc::bad_file_descriptor);
    }
    std::error_code fail(std::error_code e) noexcept
    {
        error_ = e;
        abandon();
        return e;
    }
    std::error_code drain()
    {
        if (buffer_.empty()) return {};
        auto w = detail::fs_file::write_all(fd_, buffer_.data(), buffer_.size());
        buffer_.clear();
        return w.is_err() ? fail(w.unwrap_err_unchecked()) : std::error_code{};
    }
    void take(atomic_file_writer& o) noexcept
    {
        target_ = std::move(o.target_);
        tmp_ = std::exchange(o.tmp_, {});
        buffer_ = std::exchange(o.buffer_, {});
        fd_ = std::exchange(o.fd_, -1);
        written_ = std::exchange(o.written_, 0);
        reserved_ = std::exchange(o.reserved_, 0);
        durable_ = o.durable_;
        error_ = std::exchange(o.error_, {});
    }

    std::filesystem::path target_;
    std::filesystem::path tmp_;
    std::vector<std::uint8_t> buffer_;
    int fd_ = -1;
    std::uint64_t written_ = 0;
    std::uint64_t reserved_ = 0;
    bool durable_ = true;
    std::error_code error_{};
};

namespace detail::fs_file {

inline result<void, std::error_code> write_atomically(const std::filesystem::path& path, const unsigned char* data,
                                                      std::size_t size, const write_file_options& opt)
{
    auto w = atomic_file_writer::create(path, opt);
    if (w.is_err()) return err(w.unwrap_err_unchecked());
    if (auto a = w->append(std::span(data, size)); a.is_err()) return a;
    return w->commit();
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
