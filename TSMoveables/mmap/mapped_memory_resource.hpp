//
//  mapped_memory_resource.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a std::pmr::memory_resource over a mapped_region,
//  so any pmr container, and core's object_pool, can live in a mapping:
//
//      auto region = *snicholls::mapped_region::anonymous(std::size_t{1} << 40);
//      snicholls::mapped_memory_resource mem(region);
//      snicholls::containers::object_pool<order> pool(&mem);   // chunks in the mapping
//      std::pmr::vector<double> prices(&mem);                  // so is this
//
//  ------------------------------------------------------------- allocation
//
//  Bump allocation: each allocate() rounds the cursor up to the alignment,
//  hands out the next `bytes`, and grows the region (in place, never moving:
//  mapped_region::grow) by at least `grow_step` when the cursor passes its
//  end. deallocate() is a NO-OP: memory comes back only when the resource is
//  reset() or destroyed, as with std::pmr::monotonic_buffer_resource. That is
//  the right shape for its callers - object_pool asks for a chunk at a time
//  and recycles objects inside its chunks itself, and a pmr container that
//  only grows wastes at most what its last reallocations left behind - and it
//  keeps allocate() a few instructions. A container that reallocates often
//  wants std::pmr::unsynchronized_pool_resource layered on top:
//
//      std::pmr::unsynchronized_pool_resource pooled(&mem);   // frees and reuses, upstream is the mapping
//
//  ---------------------------------------------------------------- the rules
//
//    - The region must outlive the resource, and must not move: the resource
//      only ever grows it in place, so allocations stop (std::bad_alloc) when
//      the reservation is used up. Reserve generously.
//    - allocate() throws std::bad_alloc on exhaustion or a failed grow - the
//      memory_resource interface has no other way to fail. last_error() keeps
//      the std::error_code of the last failure (ENOSPC, reservation_exhausted).
//    - Pointers handed out are only meaningful inside this process: a file-
//      backed region gives persistence of BYTES, not of the objects' pointers.
//      For structures that must be reopened, use mapped_pool (indices) or
//      core's offset_ptr.
//    - Not thread-safe; wrap it in std::pmr::synchronized_pool_resource for
//      several threads.
//
//  C++20 and POSIX, like everything under mmap/.
//

#ifndef ts_moveables_mmap_mapped_memory_resource_hpp
#define ts_moveables_mmap_mapped_memory_resource_hpp

#if !defined(_WIN32)

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <system_error>

#include "mapped_region.hpp"

namespace snicholls {

class mapped_memory_resource final : public std::pmr::memory_resource
{
public:
    // Allocate from `region`, starting at byte `offset` (for a header of the caller's own), growing it by at least
    // `grow_step` bytes at a time.
    explicit mapped_memory_resource(mapped_region& region, std::size_t offset = 0,
                                    std::size_t grow_step = std::size_t{1} << 20) noexcept
        : region_(&region), start_(offset), cursor_(offset), grow_step_(std::max(grow_step, mapped_region::page_size()))
    {
    }
    mapped_memory_resource(const mapped_memory_resource&) = delete;
    mapped_memory_resource& operator=(const mapped_memory_resource&) = delete;

    // Forget every allocation: the next one starts at the beginning again. The region keeps its size.
    void reset() noexcept { cursor_ = start_; }

    std::size_t used() const noexcept { return cursor_ - start_; }       // bytes handed out, with padding
    std::size_t available() const noexcept { return region_->reserved() - cursor_; }
    std::error_code last_error() const noexcept { return last_error_; }
    mapped_region& region() const noexcept { return *region_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        const std::size_t at = (cursor_ + alignment - 1) / alignment * alignment;
        if (at < cursor_ || at + bytes < at) return fail(make_error_code(mapped_errc::too_large));
        const std::size_t end = at + bytes;
        if (end > region_->size()) {
            const std::size_t want = std::min(std::max(end, region_->size() + grow_step_), region_->reserved());
            if (want < end) return fail(make_error_code(mapped_errc::reservation_exhausted));
            if (auto g = region_->grow(want); g.is_err()) return fail(g.unwrap_err_unchecked());
        }
        cursor_ = end;
        return region_->data() + at;
    }
    void do_deallocate(void*, std::size_t, std::size_t) noexcept override {}   // monotonic: see the header comment
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

    [[noreturn]] void* fail(std::error_code e)
    {
        last_error_ = e;
        throw std::bad_alloc();
    }

    mapped_region* region_;
    std::size_t start_;
    std::size_t cursor_;
    std::size_t grow_step_;
    std::error_code last_error_{};
};

} // namespace snicholls

#endif // !_WIN32

#endif /* ts_moveables_mmap_mapped_memory_resource_hpp */
