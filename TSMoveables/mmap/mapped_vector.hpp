//
//  mapped_vector.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a vector of trivially copyable T that lives in a
//  file, far larger than a comfortable heap, and comes back when reopened:
//
//      struct tick { std::int64_t time; double price; };
//      constexpr std::uint64_t tag = snicholls::fnv1a_64("tick v1");
//
//      auto v = snicholls::mapped_vector<tick>::create("ticks.bin", {.type_tag = tag});
//      if (v.is_err()) return v.err();
//      for (...) if (auto r = v->push_back(t); r.is_err()) return r.err();   // ENOSPC, not SIGBUS
//      v->flush();                                                          // durable, size and all
//
//      auto again = snicholls::mapped_vector<tick>::open("ticks.bin", {.type_tag = tag});
//      // again->size() is what it was at the flush; a foreign, mismatched or
//      // torn file is refused with a mapped_errc
//
//  -------------------------------------------------------------- the file
//
//      [ core's mapped_header, 64 bytes ][ T 0 ][ T 1 ] ... [ T capacity-1 ]
//
//  The header (snicholls/core/containers/mapped_header.hpp) records the
//  element's size and alignment, the caller's type tag, the size and the
//  capacity, under a wyhash checksum. The size and capacity live in the
//  header, not in the object: reopening the file restores the vector.
//  Elements are in this machine's native layout, which the shape and the tag
//  guard. T must be trivially copyable, with alignof(T) <= 64.
//
//  ---------------------------------------------------------- when it is durable
//
//  Elements are stored straight into the mapping, so they reach the page cache
//  at once and survive the process being killed. The SIZE in the header is
//  only rewritten by flush(), close() and the destructor: flush() first syncs
//  the elements, then writes the header, then syncs the header, so a header
//  on disk never counts an element that is not. After a crash the file reopens
//  with the size of the last flush (a growth in between records the new
//  capacity but not the new size). A crash in the middle of writing the header
//  leaves a checksum that does not match, and open() refuses the file rather
//  than guess.
//
//  ------------------------------------------------------------- growth
//
//  push_back doubles the capacity when full: the file is preallocated and
//  extended, and the new part mapped over the reservation, so data() does not
//  move. Only when the reservation itself is used up does the mapping move to
//  a larger one (options.allow_move, default on; off, the growth fails with
//  reservation_exhausted instead). Like std::vector, a pointer or reference
//  into the vector is valid until a growth that moves; unlike it, nothing is
//  copied when the growth does not move, which is every growth inside the
//  reservation. data() and generation() say where it is now.
//
//  ------------------------------------------------------------- the rules
//
//    - Errors are result<..., std::error_code> (errno or mapped_errc); nothing
//      throws, including on a full disk (see mapped_region.hpp).
//    - One owner, one thread. Another process may map the file read-only; it
//      sees elements as they are written and the size as of the last flush.
//    - Not shrinkable: pop_back/clear/resize lower the size, the file keeps its
//      capacity.
//
//  C++20 and POSIX, like everything under mmap/.
//

#ifndef ts_moveables_mmap_mapped_vector_hpp
#define ts_moveables_mmap_mapped_vector_hpp

#if !defined(_WIN32)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <snicholls/core/containers/mapped_header.hpp>
#include <snicholls/core/result.hpp>

#include "mapped_error.hpp"
#include "mapped_region.hpp"

namespace snicholls {

struct mapped_vector_options
{
    std::uint64_t type_tag = 0;                   // the caller's tag for T and its schema (core's fnv1a_64 of a name)
    std::size_t reserve = std::size_t{1} << 36;   // address space reserved (64 GiB): growth inside it never moves
    std::size_t initial_capacity = 0;             // create(): elements to make room for at once
    bool preallocate = true;                      // allocate disk blocks before growing (no SIGBUS on a full disk)
    bool allow_move = true;                       // past the reservation: move the mapping (true) or fail (false)
};

template <class T>
class mapped_vector
{
    static_assert(std::is_trivially_copyable_v<T>, "snicholls: mapped_vector needs a trivially copyable T (it is "
                                                   "stored byte for byte and reopened from the file)");
    static_assert(alignof(T) <= containers::mapped_header_size, "snicholls: mapped_vector elements start 64 bytes "
                                                                "into the file, so alignof(T) must be at most 64");

    static constexpr std::size_t header = containers::mapped_header_size;

public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    mapped_vector() noexcept = default;
    mapped_vector(const mapped_vector&) = delete;
    mapped_vector& operator=(const mapped_vector&) = delete;
    mapped_vector(mapped_vector&& o) noexcept { take(o); }
    mapped_vector& operator=(mapped_vector&& o) noexcept
    {
        if (this != &o) {
            (void)commit_header(size_);
            take(o);
        }
        return *this;
    }
    // Records the size in the header (no msync: the page cache writes it back; flush() first for durability)
    ~mapped_vector() { (void)commit_header(size_); }

    // A new, empty vector in `path`, replacing whatever was there
    [[nodiscard]] static result<mapped_vector, std::error_code> create(const std::string& path,
                                                                       mapped_vector_options opt = {}) noexcept
    {
        mapped_region_options ro;
        ro.reserve = opt.reserve;
        ro.create = true;
        ro.truncate = true;
        ro.preallocate = opt.preallocate;
        auto region = mapped_region::open(path, ro);
        if (region.is_err()) return err(region.unwrap_err_unchecked());
        mapped_vector v;
        v.region_ = std::move(*region);
        v.opt_ = opt;
        if (auto g = v.grow_to(std::max<std::size_t>(opt.initial_capacity, 1)); g.is_err()) return err(g.unwrap_err_unchecked());
        if (auto f = v.flush(); f.is_err()) return err(f.unwrap_err_unchecked());
        return ok(std::move(v));
    }

    // The vector in `path`, as of its last flush; refused if the header is not one of a mapped_vector<T> with this
    // options.type_tag, or is torn, or the file is shorter than the header says
    [[nodiscard]] static result<mapped_vector, std::error_code> open(const std::string& path,
                                                                     mapped_vector_options opt = {}) noexcept
    {
        mapped_region_options ro;
        ro.reserve = opt.reserve;
        ro.create = false;
        ro.preallocate = opt.preallocate;
        auto region = mapped_region::open(path, ro);
        if (region.is_err()) return err(region.unwrap_err_unchecked());
        const std::span<const std::byte> bytes = region->bytes();
        if (auto ok_shape = containers::validate_header(bytes, containers::mapped_header_for<T>(opt.type_tag));
            ok_shape.is_err())
            return err(make_error_code(to_mapped_errc(ok_shape.unwrap_err_unchecked())));
        const containers::mapped_header h = *containers::decode_header(bytes);
        if (h.capacity > (region->size() - header) / sizeof(T)) return err(make_error_code(mapped_errc::file_too_small));
        mapped_vector v;
        v.region_ = std::move(*region);
        v.opt_ = opt;
        v.data_ = elements(v.region_.data() + header, v.capacity_);
        v.size_ = v.durable_ = static_cast<std::size_t>(h.count);
        v.capacity_ = v.header_capacity_ = static_cast<std::size_t>(h.capacity);
        return ok(std::move(v));
    }

    // Append; grows the file (doubling) when full
    [[nodiscard]] result<void, std::error_code> push_back(const T& value) noexcept
    {
        if (size_ == capacity_) [[unlikely]] {
            if (auto g = grow_to(next_capacity(size_ + 1)); g.is_err()) return g;
        }
        std::memcpy(static_cast<void*>(data_ + size_), &value, sizeof(T));
        ++size_;
        return ok();
    }

    // Append a run of elements, growing once
    [[nodiscard]] result<void, std::error_code> append(std::span<const T> values) noexcept
    {
        if (values.size() > capacity_ - size_) {
            if (auto g = grow_to(next_capacity(size_ + values.size())); g.is_err()) return g;
        }
        if (!values.empty()) std::memcpy(static_cast<void*>(data_ + size_), values.data(), values.size_bytes());
        size_ += values.size();
        return ok();
    }

    // Room for at least `n` elements
    [[nodiscard]] result<void, std::error_code> reserve(std::size_t n) noexcept
    {
        if (n <= capacity_) return ok();
        return grow_to(n);
    }

    // n elements; new ones are value-initialised
    [[nodiscard]] result<void, std::error_code> resize(std::size_t n) noexcept
        requires std::is_nothrow_default_constructible_v<T>
    {
        if (n > capacity_) {
            if (auto g = grow_to(next_capacity(n)); g.is_err()) return g;
        }
        for (std::size_t i = size_; i < n; ++i) ::new (static_cast<void*>(data_ + i)) T{};
        size_ = n;
        return ok();
    }

    void pop_back() noexcept { --size_; }
    void clear() noexcept { size_ = 0; }

    // Elements, then the header, each made durable (see "when it is durable")
    [[nodiscard]] result<void, std::error_code> flush() noexcept
    {
        if (!region_.mapped()) return err(make_error_code(mapped_errc::not_mapped));
        if (size_ == durable_ && header_capacity_ == capacity_) {
            // The header on disk already says this: one msync of the elements will do, not three steps
            return size_ > 0 ? region_.flush(header, size_ * sizeof(T)) : result<void, std::error_code>(ok());
        }
        if (size_ > 0) {
            if (auto r = region_.flush(header, size_ * sizeof(T)); r.is_err()) return r;
        }
        if (auto c = commit_header(size_); c.is_err()) return c;
        if (auto r = region_.flush(0, header); r.is_err()) return r;
        durable_ = size_;
        header_capacity_ = capacity_;
        return ok();
    }

    // flush(), then unmap and close; the object is empty afterwards
    [[nodiscard]] result<void, std::error_code> close() noexcept
    {
        auto r = flush();
        region_ = mapped_region{};
        data_ = nullptr;
        size_ = capacity_ = durable_ = header_capacity_ = 0;
        return r;
    }

    T& operator[](std::size_t i) noexcept { return data_[i]; }
    const T& operator[](std::size_t i) const noexcept { return data_[i]; }
    T& back() noexcept { return data_[size_ - 1]; }
    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }
    iterator begin() noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }
    std::span<T> span() noexcept { return {data_, size_}; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t durable_size() const noexcept { return durable_; }   // the size the file reopens with after a crash
    bool empty() const noexcept { return size_ == 0; }
    unsigned generation() const noexcept { return region_.generation(); }
    mapped_region& region() noexcept { return region_; }   // for advise(), and flush of a part

private:
    // The elements in the mapped bytes. A mapping holds no C++ objects until something says so: C++23's
    // start_lifetime_as_array says so where the library has it; elsewhere trivially copyable T, bytes from an
    // opaque mmap and launder are what every compiler honours. Headers are never read this way - only through
    // core's byte-wise decode_header.
    static T* elements(std::byte* p, std::size_t n) noexcept
    {
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
        return std::start_lifetime_as_array<T>(p, n);
#else
        (void)n;
        return std::launder(reinterpret_cast<T*>(p));
#endif
    }

    std::size_t next_capacity(std::size_t at_least) const noexcept
    {
        return std::max({at_least, 2 * capacity_, std::size_t{64}});
    }

    [[nodiscard]] result<void, std::error_code> grow_to(std::size_t n) noexcept
    {
        if (n > (static_cast<std::size_t>(-1) - header) / sizeof(T)) return err(make_error_code(mapped_errc::too_large));
        // Round the file to whole pages and use them all
        const std::size_t page = mapped_region::page_size();
        const std::size_t bytes = (header + n * sizeof(T) + page - 1) / page * page;
        auto g = opt_.allow_move ? region_.grow_or_move(bytes) : region_.grow(bytes);
        if (g.is_err()) return g;
        capacity_ = (region_.size() - header) / sizeof(T);
        data_ = elements(region_.data() + header, capacity_);
        // The new capacity is recorded at once; the size stays the last durable one until flush()
        return commit_header(durable_);
    }

    result<void, std::error_code> commit_header(std::size_t count) noexcept
    {
        if (!region_.mapped()) return ok();
        auto h = containers::mapped_header_for<T>(opt_.type_tag, count, capacity_);
        if (auto e = containers::encode_header(h, region_.bytes()); e.is_err())
            return err(make_error_code(to_mapped_errc(e.unwrap_err_unchecked())));
        return ok();
    }

    void take(mapped_vector& o) noexcept
    {
        region_ = std::move(o.region_);
        opt_ = o.opt_;
        data_ = std::exchange(o.data_, nullptr);
        size_ = std::exchange(o.size_, 0);
        capacity_ = std::exchange(o.capacity_, 0);
        durable_ = std::exchange(o.durable_, 0);
        header_capacity_ = std::exchange(o.header_capacity_, 0);
    }

    mapped_region region_;
    mapped_vector_options opt_{};
    T* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    std::size_t durable_ = 0;
    std::size_t header_capacity_ = 0;   // the capacity in the header as of the last flush
};

} // namespace snicholls

#endif // !_WIN32

#endif /* ts_moveables_mmap_mapped_vector_hpp */
