//
//  mapped_region.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a memory-mapped region that grows without moving:
//  a file (persisted, shared) or anonymous memory (huge, not persisted)
//
//      auto r = snicholls::mapped_region::open("book.bin", {.reserve = 1ull << 40});
//      if (r.is_err()) return r.err();                 // std::error_code, never an exception
//      std::byte* p = r->data();                       // stays put while the region grows ...
//      if (auto g = r->grow(64 << 20); g.is_err()) ... // ... inside the reservation
//      r->advise(snicholls::map_advice::sequential);
//      r->flush();                                      // msync(MS_SYNC): durable on return
//
//  ----------------------------------------------------------- how it grows
//
//  open() reserves a large range of address space once - PROT_NONE, private,
//  anonymous, MAP_NORESERVE where the system has it, so it costs page tables
//  at most, never memory or swap - and maps the file over the start of it
//  with MAP_FIXED | MAP_SHARED. grow() extends the FILE (preallocate, then
//  ftruncate) and maps only the new tail over the reservation, again
//  MAP_FIXED. The base address never changes, so pointers into the region
//  stay valid as it grows. That is the portable form of "a vector that grows
//  in place": Linux could mremap, but macOS has no mremap, and reserving up
//  front works on both.
//
//  Growing past the reservation is refused (mapped_errc::reservation_exhausted)
//  unless the caller asks for grow_or_move(), which reserves a new, larger
//  range, maps the whole file there and releases the old one: every pointer
//  into the region is then stale, as after a std::vector reallocation.
//  Containers that hold indices (mapped_pool) or re-read data()
//  (mapped_vector) can move; a memory_resource that has handed out pointers
//  must not, so mapped_memory_resource only ever calls grow().
//
//  Reserve generously: 1 TiB of address space costs nothing on a 64-bit
//  system with 128 TiB of user address space. Sizes are rounded up to the page
//  size (4 KiB on x86-64, 16 KiB on Apple silicon).
//
//  ------------------------------------------------------ a full disk, and SIGBUS
//
//  ftruncate() makes a file longer without allocating a block for it: the new
//  part is a hole. The first write to a page of the hole needs a block, and if
//  the disk is full there is none - but a store instruction cannot return
//  ENOSPC, so the kernel sends SIGBUS, and the process dies far from the call
//  that caused it. So grow() allocates the blocks BEFORE ftruncate and before
//  any new page is touched: posix_fallocate on Linux, fcntl(F_PREALLOCATE) on
//  macOS. A full disk is then an ENOSPC from grow(), the file keeps its old
//  size, and nothing is mapped. options.preallocate = false skips it, for a
//  sparse file whose holes may stay holes, at the price of that SIGBUS.
//
//  Preallocation guards against the disk filling; it cannot guard against the
//  file being truncated by another process while it is mapped (also SIGBUS),
//  which is a usage error.
//
//  ------------------------------------------------------- durability
//
//  Stores into a MAP_SHARED mapping reach the page cache at once - another
//  process mapping the file sees them, and they survive this process being
//  killed - and the disk eventually. flush() is msync(MS_SYNC): on return
//  they are on the device (as far as the device reports). flush_async() is
//  MS_ASYNC, which schedules the write and returns. On macOS, MS_SYNC does not
//  issue F_FULLFSYNC: a power cut can still lose data in the drive's cache
//  (full_sync() does F_FULLFSYNC; on Linux it is fsync).
//
//  -------------------------------------------------------------- the rules
//
//    - One owner. Moving a region is cheap (it is a handle); the mapping does
//      not move. It is not thread-safe: grow from one thread, and do not read
//      past size() from another while it grows.
//    - Anonymous regions are zero-filled, private and die with the process.
//    - Errors are result<..., std::error_code>: errno in system_category, or
//      a mapped_errc. Nothing here throws.
//
//  C++20 and POSIX (macOS, Linux). Not built on Windows.
//

#ifndef ts_moveables_mmap_mapped_region_hpp
#define ts_moveables_mmap_mapped_region_hpp

#if !defined(_WIN32)

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <snicholls/core/result.hpp>

#include "../fs/sync.hpp"
#include "mapped_error.hpp"

namespace snicholls {

// madvise hints, by what the access pattern is about to be
enum class map_advice
{
    normal,       // MADV_NORMAL
    sequential,   // MADV_SEQUENTIAL: read ahead aggressively, drop behind
    random,       // MADV_RANDOM: no read-ahead
    willneed,     // MADV_WILLNEED: start reading it in now
    dontneed,     // MADV_DONTNEED: may be dropped (a shared file's data is kept in the file)
};

struct mapped_region_options
{
    std::size_t reserve = std::size_t{1} << 36;   // address space reserved up front (64 GiB)
    bool create = true;                           // create the file if it does not exist
    bool truncate = false;                        // start from an empty file
    bool preallocate = true;                      // allocate blocks before growing (see "a full disk")
    int mode = 0644;                              // permissions of a created file
};

class mapped_region
{
public:
    mapped_region() noexcept = default;
    mapped_region(const mapped_region&) = delete;
    mapped_region& operator=(const mapped_region&) = delete;
    mapped_region(mapped_region&& o) noexcept { take(o); }
    mapped_region& operator=(mapped_region&& o) noexcept
    {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }
    ~mapped_region() { release(); }

    // The system's page size: the unit of every mapping
    static std::size_t page_size() noexcept
    {
        static const std::size_t p = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        return p;
    }

    // Open (or create) `path` and map all of it; the reservation is at least
    // the file's size. A new or truncated file starts at size 0.
    [[nodiscard]] static result<mapped_region, std::error_code> open(const std::string& path,
                                                                     mapped_region_options opt = {}) noexcept
    {
        int flags = O_RDWR | O_CLOEXEC;
        if (opt.create) flags |= O_CREAT;
        if (opt.truncate) flags |= O_TRUNC;
        const int fd = ::open(path.c_str(), flags, opt.mode);
        if (fd < 0) return err(errno_code());
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            const auto e = errno_code();
            ::close(fd);
            return err(e);
        }
        const auto file_size = static_cast<std::size_t>(st.st_size);
        mapped_region r;
        r.fd_ = fd;
        r.preallocate_ = opt.preallocate;
        if (auto e = r.reserve_range(std::max(round_up(opt.reserve), round_up(file_size)))) return err(e);
        // A file whose size is not a page multiple is mapped to the page that holds its end; the region's size is
        // the file's, and the first grow() rounds the file up.
        if (file_size > 0) {
            if (auto e = r.map_file(0, round_up(file_size))) return err(e);
        }
        r.size_ = file_size;
        return ok(std::move(r));
    }

    // Anonymous memory: zero-filled, private, not persisted. `initial` bytes
    // are usable at once; grow() makes more of the reservation usable.
    [[nodiscard]] static result<mapped_region, std::error_code> anonymous(std::size_t reserve,
                                                                          std::size_t initial = 0) noexcept
    {
        mapped_region r;
        if (auto e = r.reserve_range(std::max(round_up(reserve), round_up(initial)))) return err(e);
        if (initial > 0) {
            if (auto g = r.grow(initial); g.is_err()) return err(g.unwrap_err_unchecked());
        }
        return ok(std::move(r));
    }

    // Make the region at least `new_size` bytes, in place. The file is
    // preallocated (unless disabled) and extended to the page-rounded size; the
    // base address does not change. Shrinking is not supported (a smaller size
    // is a no-op). On failure the region and the file are as they were.
    [[nodiscard]] result<void, std::error_code> grow(std::size_t new_size) noexcept
    {
        if (base_ == nullptr) return err(make_error_code(mapped_errc::not_mapped));
        if (new_size <= size_) return ok();
        if (new_size > reserved_) return err(make_error_code(mapped_errc::reservation_exhausted));
        const std::size_t old_mapped = round_up(size_);
        const std::size_t new_mapped = round_up(new_size);
        if (new_mapped > old_mapped) {
            if (fd_ < 0) {
                if (::mprotect(base_ + old_mapped, new_mapped - old_mapped, PROT_READ | PROT_WRITE) != 0)
                    return err(errno_code());
            } else {
                const std::size_t old_file = file_bytes_;
                if (auto e = extend_file(new_mapped)) return err(e);
                if (auto e = map_file(old_mapped, new_mapped - old_mapped)) {
                    truncate_back(fd_, old_file);
                    file_bytes_ = old_file;
                    return err(e);
                }
            }
        } else if (fd_ >= 0 && file_bytes_ < new_mapped) {
            // The file ended inside the last mapped page: extend it to the page's end before using the rest
            if (auto e = extend_file(new_mapped)) return err(e);
        }
        size_ = new_size;
        return ok();
    }

    // grow(), or if the reservation is too small, reserve a larger one (at
    // least twice the old), move the mapping there and release the old range.
    // data() changes then; generation() counts the moves.
    [[nodiscard]] result<void, std::error_code> grow_or_move(std::size_t new_size) noexcept
    {
        if (base_ == nullptr) return err(make_error_code(mapped_errc::not_mapped));
        if (new_size <= reserved_) return grow(new_size);
        mapped_region bigger;
        bigger.fd_ = -1;
        if (auto e = bigger.reserve_range(std::max(round_up(new_size), 2 * reserved_))) return err(e);
        const std::size_t mapped = round_up(size_);
        if (fd_ >= 0) {
            bigger.fd_ = fd_;
            bigger.file_bytes_ = file_bytes_;
            if (mapped > 0) {
                if (auto e = bigger.map_file(0, mapped)) {
                    bigger.fd_ = -1;   // still ours
                    return err(e);
                }
            }
        } else if (mapped > 0) {
            if (::mprotect(bigger.base_, mapped, PROT_READ | PROT_WRITE) != 0) return err(errno_code());
            std::memcpy(bigger.base_, base_, size_);
        }
        bigger.size_ = size_;
        bigger.preallocate_ = preallocate_;
        bigger.generation_ = generation_ + 1;
        // Swap in the new mapping; the old range is released without closing the shared descriptor
        ::munmap(base_, reserved_);
        fd_ = -1;
        base_ = nullptr;
        *this = std::move(bigger);
        return grow(new_size);
    }

    // msync(MS_SYNC) of [offset, offset + length), rounded out to pages; the
    // default is the whole region. Durable on return. A no-op when anonymous.
    [[nodiscard]] result<void, std::error_code> flush(std::size_t offset = 0,
                                                      std::size_t length = static_cast<std::size_t>(-1)) noexcept
    {
        return sync(offset, length, MS_SYNC);
    }
    // msync(MS_ASYNC): the write-back is scheduled, not awaited
    [[nodiscard]] result<void, std::error_code> flush_async(std::size_t offset = 0,
                                                            std::size_t length = static_cast<std::size_t>(-1)) noexcept
    {
        return sync(offset, length, MS_ASYNC);
    }
    // Through the drive's own cache too: F_FULLFSYNC on macOS, fsync on Linux
    [[nodiscard]] result<void, std::error_code> full_sync() noexcept
    {
        if (auto r = flush(); r.is_err()) return r;
        if (fd_ < 0) return ok();
        return snicholls::full_sync(fd_);
    }

    // madvise over [offset, offset + length) (default: the whole region)
    [[nodiscard]] result<void, std::error_code> advise(map_advice a, std::size_t offset = 0,
                                                       std::size_t length = static_cast<std::size_t>(-1)) noexcept
    {
        if (base_ == nullptr) return err(make_error_code(mapped_errc::not_mapped));
        auto [from, len] = page_span(offset, length);
        if (len == 0) return ok();
        int advice = MADV_NORMAL;
        switch (a) {
        case map_advice::normal: advice = MADV_NORMAL; break;
        case map_advice::sequential: advice = MADV_SEQUENTIAL; break;
        case map_advice::random: advice = MADV_RANDOM; break;
        case map_advice::willneed: advice = MADV_WILLNEED; break;
        case map_advice::dontneed: advice = MADV_DONTNEED; break;
        }
        if (::madvise(base_ + from, len, advice) != 0) return err(errno_code());
        return ok();
    }

    std::byte* data() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }             // usable bytes
    std::size_t reserved() const noexcept { return reserved_; }     // address space reserved
    std::span<std::byte> bytes() const noexcept { return {base_, size_}; }
    bool mapped() const noexcept { return base_ != nullptr; }
    bool is_anonymous() const noexcept { return base_ != nullptr && fd_ < 0; }
    int fd() const noexcept { return fd_; }
    unsigned generation() const noexcept { return generation_; }   // how many times grow_or_move moved it

private:
    // Best effort, on a path that is already failing: the error reported is the first one
    static void truncate_back(int fd, std::size_t bytes) noexcept
    {
        if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        }
    }
    static std::size_t round_up(std::size_t n) noexcept
    {
        const std::size_t p = page_size();
        return (n + p - 1) / p * p;
    }

    std::error_code reserve_range(std::size_t bytes) noexcept
    {
        if (bytes == 0) bytes = page_size();
        int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
        flags |= MAP_NORESERVE;
#endif
        void* p = ::mmap(nullptr, bytes, PROT_NONE, flags, -1, 0);
        if (p == MAP_FAILED) return errno_code();
        base_ = static_cast<std::byte*>(p);
        reserved_ = bytes;
        return {};
    }

    std::error_code map_file(std::size_t offset, std::size_t length) noexcept
    {
        void* p = ::mmap(base_ + offset, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd_,
                         static_cast<off_t>(offset));
        if (p == MAP_FAILED) return errno_code();
        struct stat st {};
        if (::fstat(fd_, &st) == 0) file_bytes_ = static_cast<std::size_t>(st.st_size);
        return {};
    }

    // Allocate the blocks first, then set the size; on failure the size is put back
    std::error_code extend_file(std::size_t new_bytes) noexcept
    {
        struct stat st {};
        if (::fstat(fd_, &st) != 0) return errno_code();
        const auto old_bytes = static_cast<std::size_t>(st.st_size);
        if (new_bytes <= old_bytes) {
            file_bytes_ = old_bytes;
            return {};
        }
        if (preallocate_) {
#if defined(__APPLE__)
            fstore_t fs{};
            fs.fst_flags = F_ALLOCATECONTIG | F_ALLOCATEALL;
            fs.fst_posmode = F_PEOFPOSMODE;   // from the physical end of the file
            fs.fst_offset = 0;
            fs.fst_length = static_cast<off_t>(new_bytes - old_bytes);
            if (::fcntl(fd_, F_PREALLOCATE, &fs) == -1) {
                fs.fst_flags = F_ALLOCATEALL;   // contiguous was too much to ask; any blocks will do
                if (::fcntl(fd_, F_PREALLOCATE, &fs) == -1) return errno_code();
            }
#else
            const int rc = ::posix_fallocate(fd_, static_cast<off_t>(old_bytes), static_cast<off_t>(new_bytes - old_bytes));
            if (rc != 0) {
                truncate_back(fd_, old_bytes);   // it may have extended the file part way
                return {rc, std::system_category()};
            }
#endif
        }
        if (::ftruncate(fd_, static_cast<off_t>(new_bytes)) != 0) {
            const auto e = errno_code();
            truncate_back(fd_, old_bytes);
            return e;
        }
        file_bytes_ = new_bytes;
        return {};
    }

    std::pair<std::size_t, std::size_t> page_span(std::size_t offset, std::size_t length) const noexcept
    {
        const std::size_t end_all = round_up(size_);
        if (offset >= end_all) return {0, 0};
        const std::size_t p = page_size();
        const std::size_t from = offset / p * p;
        const std::size_t to = (length > end_all - offset) ? end_all : round_up(offset + length);
        return {from, to - from};
    }

    result<void, std::error_code> sync(std::size_t offset, std::size_t length, int how) noexcept
    {
        if (base_ == nullptr) return err(make_error_code(mapped_errc::not_mapped));
        if (fd_ < 0) return ok();
        auto [from, len] = page_span(offset, length);
        if (len == 0) return ok();
        if (::msync(base_ + from, len, how) != 0) return err(errno_code());
        return ok();
    }

    void take(mapped_region& o) noexcept
    {
        base_ = std::exchange(o.base_, nullptr);
        size_ = std::exchange(o.size_, 0);
        reserved_ = std::exchange(o.reserved_, 0);
        file_bytes_ = std::exchange(o.file_bytes_, 0);
        fd_ = std::exchange(o.fd_, -1);
        preallocate_ = o.preallocate_;
        generation_ = o.generation_;
    }

    void release() noexcept
    {
        if (base_ != nullptr) ::munmap(base_, reserved_);
        if (fd_ >= 0) ::close(fd_);
        base_ = nullptr;
        fd_ = -1;
        size_ = reserved_ = file_bytes_ = 0;
    }

    std::byte* base_ = nullptr;
    std::size_t size_ = 0;
    std::size_t reserved_ = 0;
    std::size_t file_bytes_ = 0;
    int fd_ = -1;
    bool preallocate_ = true;
    unsigned generation_ = 0;
};

} // namespace snicholls

#endif // !_WIN32

#endif /* ts_moveables_mmap_mapped_region_hpp */
