//
//  mapped_error.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the errors of the memory-mapped containers, as a
//  std::error_code category, so one result<..., std::error_code> carries an
//  operating-system error (errno, in std::system_category) and a refusal of
//  the file's contents alike:
//
//      auto v = snicholls::mapped_vector<tick>::open("ticks.bin");
//      if (v.is_err()) {
//          if (v.err() == snicholls::mapped_errc::bad_checksum) ...   // torn or corrupted header
//          if (v.err() == std::errc::no_space_on_device) ...         // the disk is full
//      }
//
//  Core's header_error and index_pool_error convert one to one
//  (to_mapped_errc), so nothing is lost on the way up.
//
//  C++20 and POSIX, like everything under mmap/.
//

#ifndef ts_moveables_mmap_mapped_error_hpp
#define ts_moveables_mmap_mapped_error_hpp

#include <string>
#include <system_error>

#include <snicholls/core/containers/index_pool.hpp>
#include <snicholls/core/containers/mapped_header.hpp>

namespace snicholls {

// Every way the mapped containers refuse, beyond what errno says
enum class mapped_errc : int
{
    reservation_exhausted = 1,   // a growth past the reserved address range where moving is not allowed
    too_large,                   // a size that overflows, or more elements than the index type can name
    not_mapped,                  // an operation on a moved-from or default-constructed object
    file_too_small,              // the file is shorter than its header says, or than a header
    // core's mapped_header, one to one
    header_too_short,
    bad_magic,
    bad_version,
    bad_header_size,
    bad_checksum,
    count_over_capacity,
    element_mismatch,
    type_mismatch,
    // core's index_pool, one to one
    pool_misaligned,
    pool_too_small,
    pool_full,
    pool_not_formatted,
    pool_layout_mismatch,
    pool_corrupt,
    // this library's own again
    read_only,                   // a growth of a region mapped read-only
};

} // namespace snicholls

// Declared before any use, so a mapped_errc converts to std::error_code everywhere below
template <>
struct std::is_error_code_enum<snicholls::mapped_errc> : std::true_type
{
};

namespace snicholls {

namespace detail {
    class mapped_category_impl final : public std::error_category
    {
    public:
        const char* name() const noexcept override { return "snicholls.mapped"; }
        std::string message(int c) const override
        {
            switch (static_cast<mapped_errc>(c)) {
            case mapped_errc::reservation_exhausted: return "growth past the reserved address range";
            case mapped_errc::too_large: return "size too large";
            case mapped_errc::not_mapped: return "nothing is mapped";
            case mapped_errc::file_too_small: return "file shorter than its header says";
            case mapped_errc::header_too_short: return "header: fewer than 64 bytes";
            case mapped_errc::bad_magic: return "header: not a mapped container file";
            case mapped_errc::bad_version: return "header: unsupported format version";
            case mapped_errc::bad_header_size: return "header: unexpected header size";
            case mapped_errc::bad_checksum: return "header: checksum mismatch (torn or corrupted)";
            case mapped_errc::count_over_capacity: return "header: count exceeds capacity";
            case mapped_errc::element_mismatch: return "header: element size or alignment differs";
            case mapped_errc::type_mismatch: return "header: type tag differs";
            case mapped_errc::pool_misaligned: return "pool: region misaligned";
            case mapped_errc::pool_too_small: return "pool: region too small";
            case mapped_errc::pool_full: return "pool: full";
            case mapped_errc::pool_not_formatted: return "pool: region is not a pool";
            case mapped_errc::pool_layout_mismatch: return "pool: slot size or index width differs";
            case mapped_errc::pool_corrupt: return "pool: counters contradict each other";
            case mapped_errc::read_only: return "the region is mapped read-only";
            }
            return "unknown mapped error";
        }
    };
} // namespace detail

inline const std::error_category& mapped_category() noexcept
{
    static const detail::mapped_category_impl c;
    return c;
}

inline std::error_code make_error_code(mapped_errc e) noexcept { return {static_cast<int>(e), mapped_category()}; }

inline mapped_errc to_mapped_errc(containers::header_error e) noexcept
{
    using H = containers::header_error;
    switch (e) {
    case H::too_short: return mapped_errc::header_too_short;
    case H::bad_magic: return mapped_errc::bad_magic;
    case H::bad_version: return mapped_errc::bad_version;
    case H::bad_size: return mapped_errc::bad_header_size;
    case H::bad_checksum: return mapped_errc::bad_checksum;
    case H::count_over_capacity: return mapped_errc::count_over_capacity;
    case H::element_mismatch: return mapped_errc::element_mismatch;
    case H::type_mismatch: return mapped_errc::type_mismatch;
    }
    return mapped_errc::bad_magic;
}

inline mapped_errc to_mapped_errc(containers::index_pool_error e) noexcept
{
    using P = containers::index_pool_error;
    switch (e) {
    case P::misaligned: return mapped_errc::pool_misaligned;
    case P::too_small: return mapped_errc::pool_too_small;
    case P::full: return mapped_errc::pool_full;
    case P::not_formatted: return mapped_errc::pool_not_formatted;
    case P::layout_mismatch: return mapped_errc::pool_layout_mismatch;
    case P::corrupt: return mapped_errc::pool_corrupt;
    }
    return mapped_errc::pool_corrupt;
}

} // namespace snicholls

#endif /* ts_moveables_mmap_mapped_error_hpp */
