//
//  sync.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - push a file descriptor's data through the drive's
//  own cache, the one place in the library that knows how:
//
//      if (auto r = snicholls::full_sync(fd); r.is_err()) return r;
//
//  fsync() on Linux reaches stable storage. On macOS it only reaches the
//  drive, whose cache may still lose the data in a power cut; F_FULLFSYNC asks
//  the drive to flush that cache too. Some filesystems refuse F_FULLFSYNC
//  (network and FUSE mounts answer ENOTSUP or EINVAL), and there the best on
//  offer is fsync(), so that is what is used - as SQLite does.
//
//  A directory descriptor can be synced the same way, which is what makes a
//  rename durable (write_file_atomically in file.hpp).
//
//  C++20 and POSIX, like mmap/.
//

#ifndef ts_moveables_fs_sync_hpp
#define ts_moveables_fs_sync_hpp

#if !defined(_WIN32)

#include <cerrno>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <snicholls/core/result.hpp>

namespace snicholls {

// errno as a std::error_code in the system category
[[nodiscard]] inline std::error_code errno_code() noexcept { return {errno, std::system_category()}; }

// Sync `fd`'s data and metadata to stable storage: F_FULLFSYNC on macOS (fsync where the filesystem refuses it),
// fsync elsewhere. EINTR is retried.
[[nodiscard]] inline result<void, std::error_code> full_sync(int fd) noexcept
{
#if defined(__APPLE__)
    for (;;) {
        if (::fcntl(fd, F_FULLFSYNC) == 0) return ok();
        if (errno == EINTR) continue;
        if (errno != ENOTSUP && errno != EINVAL) return err(errno_code());
        break;   // the filesystem has no F_FULLFSYNC: fall through to fsync
    }
#endif
    for (;;) {
        if (::fsync(fd) == 0) return ok();
        if (errno != EINTR) return err(errno_code());
    }
}

} // namespace snicholls

#endif // !_WIN32

#endif // ts_moveables_fs_sync_hpp
