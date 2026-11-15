//
//  mapped_pool.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a persistent object pool: core's index_pool in a
//  memory-mapped file, reopened from its header with every record, index and
//  free slot as it was:
//
//      auto pool = snicholls::mapped_pool<order>::create("orders.bin", {.type_tag = tag});
//      std::uint32_t id = *pool->insert(order{...});   // an index, not a pointer
//      pool->erase(id);                                // back on the free list
//      pool->flush();
//
//      auto again = snicholls::mapped_pool<order>::open("orders.bin", {.type_tag = tag});
//      (*again)[id] ...                                // the same record, at whatever address
//
//  ---------------------------------------------------------------- the file
//
//      [ core's mapped_header, 64 bytes ][ index_pool state, 64 ][ slot 0 ][ slot 1 ] ...
//
//  The header holds T's shape, the caller's tag, the live count, the capacity
//  and, in its user word, the high-water mark; the index_pool's own state
//  (free list head and counters) follows it. open() checks the header, then
//  attaches the pool, which checks its counters against each other and
//  against the file.
//
//  Records are addressed by index, so the pool may move when it outgrows its
//  reservation (grow_or_move and index_pool::rebase), and links between
//  records stored as indices stay valid in the file.
//
//  --------------------------------------------------------- durability
//
//  As for mapped_vector: records and the pool's state are stores into the
//  mapping, which survive this process being killed; flush() syncs them, then
//  rewrites the header and syncs it. Unlike mapped_vector the pool's own state
//  is in the mapping and changes with every insert, so after an OS crash
//  between flushes the state on disk may be newer or older than the header's
//  counts; open() trusts the pool's state (the header is the guard against a
//  foreign or torn file, not a second copy of the counters).
//
//  C++20 and POSIX, like everything under mmap/.
//

#ifndef ts_moveables_mmap_mapped_pool_hpp
#define ts_moveables_mmap_mapped_pool_hpp

#if !defined(_WIN32)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <snicholls/core/containers/index_pool.hpp>
#include <snicholls/core/containers/mapped_header.hpp>
#include <snicholls/core/result.hpp>

#include "mapped_error.hpp"
#include "mapped_region.hpp"

namespace snicholls {

struct mapped_pool_options
{
    std::uint64_t type_tag = 0;                   // the caller's tag for T and its schema
    std::size_t reserve = std::size_t{1} << 36;   // address space reserved; growth past it moves the mapping
    std::size_t initial_capacity = 0;             // create(): slots to make room for at once
    bool preallocate = true;                      // allocate disk blocks before growing
};

template <class T, class Index = std::uint32_t>
class mapped_pool
{
    using pool_type = containers::index_pool<T, Index>;
    static constexpr std::size_t header = containers::mapped_header_size;
    static_assert(pool_type::region_alignment <= header, "snicholls: mapped_pool slots must align within 64 bytes");

public:
    using value_type = T;
    using index_type = Index;
    static constexpr Index npos = pool_type::npos;   // names no slot: a null link

    mapped_pool() noexcept = default;
    mapped_pool(const mapped_pool&) = delete;
    mapped_pool& operator=(const mapped_pool&) = delete;
    mapped_pool(mapped_pool&& o) noexcept
        : region_(std::move(o.region_)), pool_(std::exchange(o.pool_, pool_type{})), opt_(o.opt_)
    {
    }
    mapped_pool& operator=(mapped_pool&& o) noexcept
    {
        if (this != &o) {
            (void)commit_header();
            region_ = std::move(o.region_);
            pool_ = std::exchange(o.pool_, pool_type{});
            opt_ = o.opt_;
        }
        return *this;
    }
    ~mapped_pool() { (void)commit_header(); }

    // A new, empty pool in `path`, replacing whatever was there
    [[nodiscard]] static result<mapped_pool, std::error_code> create(const std::string& path,
                                                                     mapped_pool_options opt = {}) noexcept
    {
        mapped_region_options ro;
        ro.reserve = opt.reserve;
        ro.truncate = true;
        ro.preallocate = opt.preallocate;
        auto region = mapped_region::open(path, ro);
        if (region.is_err()) return err(region.unwrap_err_unchecked());
        mapped_pool p;
        p.region_ = std::move(*region);
        p.opt_ = opt;
        if (auto g = p.region_.grow(bytes_for(std::max<std::size_t>(opt.initial_capacity, 1))); g.is_err())
            return err(g.unwrap_err_unchecked());
        auto made = pool_type::format(p.pool_bytes());
        if (made.is_err()) return err(make_error_code(to_mapped_errc(made.unwrap_err_unchecked())));
        p.pool_ = *made;
        if (auto f = p.flush(); f.is_err()) return err(f.unwrap_err_unchecked());
        return ok(std::move(p));
    }

    // The pool in `path`; refused if the header is not one of a mapped_pool<T> with this options.type_tag, is
    // torn, or the pool's state contradicts itself or the file
    [[nodiscard]] static result<mapped_pool, std::error_code> open(const std::string& path,
                                                                   mapped_pool_options opt = {}) noexcept
    {
        mapped_region_options ro;
        ro.reserve = opt.reserve;
        ro.create = false;
        ro.preallocate = opt.preallocate;
        auto region = mapped_region::open(path, ro);
        if (region.is_err()) return err(region.unwrap_err_unchecked());
        if (auto v = containers::validate_header(region->bytes(), containers::mapped_header_for<T>(opt.type_tag));
            v.is_err())
            return err(make_error_code(to_mapped_errc(v.unwrap_err_unchecked())));
        mapped_pool p;
        p.region_ = std::move(*region);
        p.opt_ = opt;
        auto attached = pool_type::attach(p.pool_bytes());
        if (attached.is_err()) return err(make_error_code(to_mapped_errc(attached.unwrap_err_unchecked())));
        p.pool_ = *attached;
        return ok(std::move(p));
    }

    // Store `value` in a free slot, growing the file (doubling) when every slot is in use
    [[nodiscard]] result<Index, std::error_code> insert(const T& value) noexcept
    {
        auto r = pool_.create(value);
        if (r.is_ok()) [[likely]]
            return ok(*r);
        if (auto g = grow(); g.is_err()) return err(g.unwrap_err_unchecked());
        auto again = pool_.create(value);
        if (again.is_err()) return err(make_error_code(to_mapped_errc(again.unwrap_err_unchecked())));
        return ok(*again);
    }

    // Put slot `i` back on the free list
    void erase(Index i) noexcept { pool_.destroy(i); }

    T& operator[](Index i) noexcept { return pool_[i]; }
    const T& operator[](Index i) const noexcept { return pool_[i]; }

    // Records and the pool's state, then the header, each made durable
    [[nodiscard]] result<void, std::error_code> flush() noexcept
    {
        if (!region_.mapped()) return err(make_error_code(mapped_errc::not_mapped));
        if (auto r = region_.flush(); r.is_err()) return r;
        if (auto c = commit_header(); c.is_err()) return c;
        return region_.flush(0, header);
    }

    std::size_t live() const noexcept { return pool_.live(); }
    std::size_t capacity() const noexcept { return pool_.capacity(); }
    std::size_t high_water() const noexcept { return pool_.high_water(); }
    mapped_region& region() noexcept { return region_; }

private:
    static constexpr std::size_t bytes_for(std::size_t slots) noexcept { return header + pool_type::bytes_for(slots); }
    std::span<std::byte> pool_bytes() noexcept { return region_.bytes().subspan(header); }

    result<void, std::error_code> grow() noexcept
    {
        const std::size_t slots = std::max<std::size_t>(2 * pool_.capacity(), 64);
        if (slots > static_cast<std::size_t>(pool_type::npos)) return err(make_error_code(mapped_errc::too_large));
        const std::size_t page = mapped_region::page_size();
        const std::size_t bytes = (bytes_for(slots) + page - 1) / page * page;
        if (auto g = region_.grow_or_move(bytes); g.is_err()) return g;
        if (auto r = pool_.rebase(pool_bytes()); r.is_err())
            return err(make_error_code(to_mapped_errc(r.unwrap_err_unchecked())));
        return commit_header();
    }

    result<void, std::error_code> commit_header() noexcept
    {
        if (!region_.mapped() || !pool_.attached()) return ok();
        auto h = containers::mapped_header_for<T>(opt_.type_tag, pool_.live(), pool_.capacity());
        h.user = pool_.high_water();
        if (auto e = containers::encode_header(h, region_.bytes()); e.is_err())
            return err(make_error_code(to_mapped_errc(e.unwrap_err_unchecked())));
        return ok();
    }

    mapped_region region_;
    pool_type pool_{};
    mapped_pool_options opt_{};
};

} // namespace snicholls

#endif // !_WIN32

#endif /* ts_moveables_mmap_mapped_pool_hpp */
