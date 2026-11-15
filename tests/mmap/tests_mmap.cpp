//
//  tests_mmap.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - tests for mmap/: mapped_region, mapped_memory_resource, mapped_vector and mapped_pool.
//  Their own program (C++20 and POSIX, where the library's main suite also builds as C++17), run by `make test-mmap`:
//
//    - create, grow past the first reservation (so the mapping moves), flush, close, reopen, check every element
//    - a child process killed (SIGKILL) after a flush and more unflushed pushes; the parent reopens
//    - a corrupted header, a foreign tag, a different element type and a truncated file are refused
//    - core's object_pool and a pmr vector on a mapped_memory_resource
//    - a full disk is ENOSPC from push_back, never SIGBUS: set TSM_MMAP_SMALL_FS to a small filesystem (Linux:
//      a tmpfs, `docker run --tmpfs /small:size=1m`); skipped when unset
//
//  Files go in $TMPDIR (or /tmp), in a directory of their own, removed at the end.
//

#include "../test_helpers.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory_resource>
#include <numeric>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <snicholls/core/containers/object_pool.hpp>
#include <snicholls/core/hash/fnv.hpp>

#include "mmap/mapped_memory_resource.hpp"
#include "mmap/mapped_pool.hpp"
#include "mmap/mapped_region.hpp"
#include "mmap/mapped_vector.hpp"

namespace {

using snicholls::mapped_errc;
using snicholls::mapped_memory_resource;
using snicholls::mapped_pool;
using snicholls::mapped_region;
using snicholls::mapped_vector;

std::string dir;

std::string path(const char* name) { return dir + "/" + name; }

struct tick
{
    std::int64_t time;
    double price;
    std::int32_t size;
    std::int32_t venue;
};
static_assert(sizeof(tick) == 24);

constexpr std::uint64_t tick_tag = snicholls::fnv1a_64("tick v1");

tick make_tick(std::size_t i)
{
    return tick{static_cast<std::int64_t>(i) * 1000, 100.0 + static_cast<double>(i % 977) * 0.01,
                static_cast<std::int32_t>(i % 500), static_cast<std::int32_t>(i % 7)};
}

bool same(const tick& a, const tick& b)
{
    return a.time == b.time && a.price == b.price && a.size == b.size && a.venue == b.venue;
}

void test_region_anonymous_grows_in_place()
{
    const std::size_t page = mapped_region::page_size();
    auto r = mapped_region::anonymous(16 * page, page);
    assert(r.is_ok());
    assert(r->is_anonymous() && r->size() == page && r->reserved() == 16 * page);
    std::byte* base = r->data();
    std::memset(base, 0x5a, page);
    assert(r->grow(10 * page).is_ok());
    assert(r->data() == base);                          // in place
    assert(r->data()[page] == std::byte{0});            // zero-filled
    assert(r->data()[0] == std::byte{0x5a});
    std::memset(base + page, 1, 9 * page);              // the new part is writable
    assert(r->grow(17 * page).err() == mapped_errc::reservation_exhausted);
    assert(r->size() == 10 * page);
    assert(r->grow(5 * page).is_ok() && r->size() == 10 * page);   // smaller is a no-op
    assert(r->flush().is_ok());                         // nothing to sync for anonymous memory
    assert(r->advise(snicholls::map_advice::random).is_ok());
    pass("mapped_region: anonymous memory grows in place, zero-filled, and refuses past its reservation");
}

void test_region_grow_or_move_keeps_contents()
{
    const std::size_t page = mapped_region::page_size();
    auto r = mapped_region::anonymous(2 * page, 2 * page);
    assert(r.is_ok());
    for (std::size_t i = 0; i < 2 * page; ++i) r->data()[i] = static_cast<std::byte>(i * 31);
    assert(r->grow_or_move(5 * page).is_ok());
    assert(r->generation() == 1 && r->reserved() >= 5 * page && r->size() == 5 * page);
    for (std::size_t i = 0; i < 2 * page; ++i) assert(r->data()[i] == static_cast<std::byte>(i * 31));
    pass("mapped_region: grow_or_move past the reservation moves, keeping every byte");
}

void test_region_file_round_trip()
{
    const std::size_t page = mapped_region::page_size();
    const auto file = path("region.bin");
    {
        auto r = mapped_region::open(file, {.reserve = 64 * page, .truncate = true});
        assert(r.is_ok());
        assert(r->size() == 0 && !r->is_anonymous() && r->fd() >= 0);
        std::byte* base = r->data();
        assert(r->grow(3 * page + 10).is_ok());
        assert(r->data() == base && r->size() == 3 * page + 10);
        assert(std::filesystem::file_size(file) == 4 * page);   // the file is whole pages
        for (std::size_t i = 0; i < r->size(); ++i) r->data()[i] = static_cast<std::byte>(i ^ 0x33);
        assert(r->flush().is_ok());
        assert(r->flush_async(page, page).is_ok());
        assert(r->full_sync().is_ok());
        assert(r->advise(snicholls::map_advice::sequential).is_ok());
        assert(r->advise(snicholls::map_advice::willneed, page, 2 * page).is_ok());
    }
    auto again = mapped_region::open(file, {.create = false});
    assert(again.is_ok());
    assert(again->size() == 4 * page);
    for (std::size_t i = 0; i < 3 * page + 10; ++i) assert(again->data()[i] == static_cast<std::byte>(i ^ 0x33));
    assert(mapped_region::open(path("missing.bin"), {.create = false}).err() == std::errc::no_such_file_or_directory);
    mapped_region empty;
    assert(!empty.mapped() && empty.grow(1).err() == mapped_errc::not_mapped);
    pass("mapped_region: a file grows in place, flushes, and reopens with its bytes");
}

void test_region_file_moves_past_its_reservation()
{
    const std::size_t page = mapped_region::page_size();
    const auto file = path("region_move.bin");
    auto r = mapped_region::open(file, {.reserve = 2 * page, .truncate = true});
    assert(r.is_ok());
    assert(r->grow(2 * page).is_ok());
    std::memset(r->data(), 0x77, 2 * page);
    assert(r->grow(3 * page).err() == mapped_errc::reservation_exhausted);
    assert(r->grow_or_move(8 * page).is_ok());
    assert(r->generation() == 1 && r->size() == 8 * page);
    assert(r->data()[2 * page - 1] == std::byte{0x77} && r->data()[2 * page] == std::byte{0});
    r->data()[8 * page - 1] = std::byte{1};
    assert(r->flush().is_ok());
    assert(std::filesystem::file_size(file) == 8 * page);
    pass("mapped_region: a file region moves to a larger reservation with its contents");
}

void test_resource_object_pool_and_pmr_vector()
{
    auto region = mapped_region::anonymous(std::size_t{1} << 30);
    assert(region.is_ok());
    {
        mapped_memory_resource mem(*region, 0, 1 << 16);
        struct order
        {
            long id, quantity;
            order(long i, long q) noexcept : id(i), quantity(q) {}
        };
        snicholls::containers::object_pool<order, 1024> pool(&mem);
        std::vector<order*> made;
        for (long i = 0; i < 10000; ++i) made.push_back(pool.create(i, 2 * i));
        for (order* o : made) {
            const auto* p = reinterpret_cast<const std::byte*>(o);
            assert(p >= region->data() && p < region->data() + region->size());
        }
        for (long i = 0; i < 10000; ++i) assert(made[static_cast<std::size_t>(i)]->quantity == 2 * i);
        for (order* o : made) pool.destroy(o);

        std::pmr::vector<double> prices(&mem);
        for (int i = 0; i < 100000; ++i) prices.push_back(i * 0.5);
        assert(prices[99999] == 99999 * 0.5);
        const auto* p = reinterpret_cast<const std::byte*>(prices.data());
        assert(p >= region->data() && p < region->data() + region->size());
        assert(mem.used() > 100000 * sizeof(double));
        assert(mem.is_equal(mem));
        mem.reset();
        assert(mem.used() == 0);
    }
    // Exhaustion is std::bad_alloc, with the reason kept
    auto small = mapped_region::anonymous(mapped_region::page_size() * 2);
    assert(small.is_ok());
    mapped_memory_resource tiny(*small);
    bool threw = false;
    try {
        (void)tiny.allocate(mapped_region::page_size() * 4, 8);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    assert(threw && tiny.last_error() == mapped_errc::reservation_exhausted);
    void* a = tiny.allocate(100, 64);
    assert(reinterpret_cast<std::uintptr_t>(a) % 64 == 0);
    tiny.deallocate(a, 100, 64);   // a no-op: monotonic
    pass("mapped_memory_resource: core's object_pool and a pmr vector live in the mapping; exhaustion is bad_alloc");
}

void test_vector_round_trip_past_the_reservation()
{
    const auto file = path("ticks.bin");
    constexpr std::size_t n = 300000;   // 7.2 MB through a 256 KiB reservation: the mapping moves several times
    std::uintptr_t first_data = 0;
    {
        auto v = mapped_vector<tick>::create(file, {.type_tag = tick_tag, .reserve = 256 * 1024});
        assert(v.is_ok());
        assert(v->empty() && v->capacity() > 0);
        first_data = reinterpret_cast<std::uintptr_t>(v->data());
        for (std::size_t i = 0; i < n; ++i) assert(v->push_back(make_tick(i)).is_ok());
        assert(v->size() == n && v->capacity() >= n);
        assert(v->generation() > 0);   // it outgrew the reservation and moved
        assert(reinterpret_cast<std::uintptr_t>(v->data()) != first_data);
        for (std::size_t i = 0; i < n; ++i) assert(same((*v)[i], make_tick(i)));
        assert(v->flush().is_ok());
        assert(v->durable_size() == n);
        assert(v->close().is_ok());
        assert(v->size() == 0 && v->data() == nullptr);
    }
    auto again = mapped_vector<tick>::open(file, {.type_tag = tick_tag});
    assert(again.is_ok());
    assert(again->size() == n);
    std::size_t i = 0;
    for (const tick& t : *again) assert(same(t, make_tick(i++)));
    assert(i == n);
    // And it carries on from where it was
    assert(again->push_back(make_tick(n)).is_ok());
    assert(same(again->back(), make_tick(n)));
    pass("mapped_vector: 300,000 elements past the first reservation, flushed, closed, reopened, each one checked");
}

void test_vector_growth_inside_the_reservation_never_moves()
{
    auto v = mapped_vector<std::uint64_t>::create(path("u64.bin"), {.reserve = std::size_t{1} << 30});
    assert(v.is_ok());
    const std::uint64_t* base = v->data();
    for (std::uint64_t i = 0; i < 1000000; ++i) assert(v->push_back(i * i).is_ok());
    assert(v->data() == base && v->generation() == 0);
    std::vector<std::uint64_t> more(1000);
    std::iota(more.begin(), more.end(), std::uint64_t{7});
    assert(v->append(more).is_ok());
    assert(v->size() == 1001000 && (*v)[1000999] == 1006);
    assert(v->resize(1001005).is_ok() && (*v)[1001004] == 0);
    v->pop_back();
    assert(v->size() == 1001004);
    assert(v->reserve(3000000).is_ok() && v->capacity() >= 3000000 && v->data() == base);
    assert(v->region().advise(snicholls::map_advice::sequential).is_ok());
    v->clear();
    assert(v->empty());
    // With moving forbidden, the reservation is a hard limit
    auto fixed = mapped_vector<std::uint64_t>::create(path("fixed.bin"), {.reserve = 64 * 1024, .allow_move = false});
    assert(fixed.is_ok());
    std::error_code last;
    for (int i = 0; i < 100000 && !last; ++i)
        if (auto r = fixed->push_back(1); r.is_err()) last = r.unwrap_err_unchecked();
    assert(last == mapped_errc::reservation_exhausted);
    assert(fixed->size() == fixed->capacity());
    pass("mapped_vector: growth inside the reservation keeps data(); with allow_move off it stops at the reservation");
}

void test_vector_survives_a_kill_after_flush()
{
    const auto file = path("crash.bin");
    constexpr std::size_t flushed = 50000, unflushed = 200000;
    std::fflush(nullptr);
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        auto v = mapped_vector<tick>::create(file, {.type_tag = tick_tag});
        if (v.is_err()) ::_exit(2);
        for (std::size_t i = 0; i < flushed; ++i)
            if (v->push_back(make_tick(i)).is_err()) ::_exit(3);
        if (v->flush().is_err()) ::_exit(4);
        // More, past several growths, never flushed: the header still says `flushed`
        for (std::size_t i = flushed; i < flushed + unflushed; ++i)
            if (v->push_back(make_tick(i)).is_err()) ::_exit(5);
        ::kill(::getpid(), SIGKILL);   // no destructor, no flush
        ::_exit(6);
    }
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    auto v = mapped_vector<tick>::open(file, {.type_tag = tick_tag});
    assert(v.is_ok());
    assert(v->size() == flushed);
    assert(v->capacity() >= flushed + unflushed);   // the growths were recorded
    for (std::size_t i = 0; i < flushed; ++i) assert(same((*v)[i], make_tick(i)));
    pass("mapped_vector: a child killed after flush leaves a file that reopens with the flushed elements");
}

void corrupt_byte(const std::string& file, off_t at)
{
    const int fd = ::open(file.c_str(), O_RDWR);
    assert(fd >= 0);
    unsigned char b = 0;
    assert(::pread(fd, &b, 1, at) == 1);
    b ^= 0x10;
    assert(::pwrite(fd, &b, 1, at) == 1);
    ::close(fd);
}

void test_vector_refuses_bad_headers()
{
    const auto file = path("refuse.bin");
    {
        auto v = mapped_vector<tick>::create(file, {.type_tag = tick_tag});
        assert(v.is_ok());
        for (std::size_t i = 0; i < 1000; ++i) assert(v->push_back(make_tick(i)).is_ok());
        assert(v->flush().is_ok());
    }
    assert(mapped_vector<tick>::open(file, {.type_tag = tick_tag}).is_ok());
    assert(mapped_vector<tick>::open(file, {.type_tag = tick_tag + 1}).err() == mapped_errc::type_mismatch);
    assert(mapped_vector<double>::open(file, {.type_tag = tick_tag}).err() == mapped_errc::element_mismatch);

    corrupt_byte(file, 33);   // inside the count
    auto torn = mapped_vector<tick>::open(file, {.type_tag = tick_tag});
    assert(torn.err() == mapped_errc::bad_checksum);
    assert(torn.unwrap_err_unchecked().message().find("checksum") != std::string::npos);
    corrupt_byte(file, 33);   // put it back
    assert(mapped_vector<tick>::open(file, {.type_tag = tick_tag}).is_ok());
    corrupt_byte(file, 0);
    assert(mapped_vector<tick>::open(file, {.type_tag = tick_tag}).err() == mapped_errc::bad_magic);
    corrupt_byte(file, 0);

    // Shorter than the capacity the header claims
    assert(::truncate(file.c_str(), static_cast<off_t>(mapped_region::page_size())) == 0);
    assert(mapped_vector<tick>::open(file, {.type_tag = tick_tag}).err() == mapped_errc::file_too_small);
    // Shorter than a header, and empty
    assert(::truncate(file.c_str(), 10) == 0);
    assert(mapped_vector<tick>::open(file, {.type_tag = tick_tag}).err() == mapped_errc::header_too_short);
    assert(::truncate(file.c_str(), 0) == 0);
    assert(mapped_vector<tick>::open(file, {.type_tag = tick_tag}).err() == mapped_errc::header_too_short);
    // A file of something else entirely
    {
        std::FILE* f = std::fopen(file.c_str(), "wb");
        std::vector<char> junk(8192, 'x');
        std::fwrite(junk.data(), 1, junk.size(), f);
        std::fclose(f);
    }
    assert(mapped_vector<tick>::open(file, {.type_tag = tick_tag}).err() == mapped_errc::bad_magic);
    pass("mapped_vector: a corrupted, foreign, mistyped, mistagged or truncated file is refused");
}

struct node
{
    std::uint64_t key;
    std::uint32_t next;   // a link stored as an index: valid in the file
    std::uint32_t value;
};

void test_pool_persists_and_grows()
{
    const auto file = path("pool.bin");
    constexpr std::uint64_t tag = snicholls::fnv1a_64("node v1");
    std::vector<std::uint32_t> ids;
    {
        auto p = mapped_pool<node>::create(file, {.type_tag = tag, .reserve = 64 * 1024});
        assert(p.is_ok());
        std::uint32_t prev = mapped_pool<node>::npos;
        for (std::uint32_t i = 0; i < 20000; ++i) {
            auto id = p->insert(node{i * 11u, prev, i});
            assert(id.is_ok());
            ids.push_back(*id);
            prev = *id;
        }
        assert(p->live() == 20000 && p->region().generation() > 0);   // grew past its reservation and moved
        for (std::uint32_t i = 0; i < 20000; i += 2) p->erase(ids[i]);
        assert(p->live() == 10000);
        assert(p->flush().is_ok());
    }
    auto p = mapped_pool<node>::open(file, {.type_tag = tag});
    assert(p.is_ok());
    assert(p->live() == 10000 && p->high_water() == 20000);
    for (std::uint32_t i = 1; i < 20000; i += 2) assert((*p)[ids[i]].value == i && (*p)[ids[i]].key == i * 11u);
    // The free list survived: the last erased slot is reused first
    assert(*p->insert(node{1, 0, 1}) == ids[19998]);
    // Follow the links from the newest live record back
    std::uint32_t at = ids[19999], steps = 0;
    while (at != mapped_pool<node>::npos && steps < 3) {
        at = (*p)[at].next;
        ++steps;
    }
    assert(steps == 3);
    assert(mapped_pool<node>::open(file, {.type_tag = tag + 1}).err() == mapped_errc::type_mismatch);
    assert(mapped_pool<tick>::open(file, {.type_tag = tag}).err() == mapped_errc::element_mismatch);
    pass("mapped_pool: core's index_pool in a file grows past its reservation, persists, and reopens with its free list");
}

void test_pool_refuses_a_corrupt_state()
{
    const auto file = path("pool_bad.bin");
    {
        auto p = mapped_pool<node>::create(file, {});
        assert(p.is_ok());
        for (std::uint32_t i = 0; i < 10; ++i) assert(p->insert(node{i, 0, i}).is_ok());
        assert(p->flush().is_ok());
    }
    // The pool's live count (word 5 of its state, after the 64-byte header) beyond its high-water mark
    const int fd = ::open(file.c_str(), O_RDWR);
    const std::uint64_t bogus = 99;
    assert(::pwrite(fd, &bogus, 8, 64 + 5 * 8) == 8);
    ::close(fd);
    assert(mapped_pool<node>::open(file, {}).err() == mapped_errc::pool_corrupt);
    pass("mapped_pool: a pool state that contradicts itself is refused");
}

void test_full_disk_is_an_error_not_sigbus()
{
    const char* small = std::getenv("TSM_MMAP_SMALL_FS");
    if (!small || !*small) {
        std::cout << "SKIP  full disk: set TSM_MMAP_SMALL_FS to a small filesystem to run it\n";
        return;
    }
    const std::string file = std::string(small) + "/full.bin";
    auto v = mapped_vector<std::uint64_t>::create(file, {});
    assert(v.is_ok());
    std::error_code last;
    std::size_t pushed = 0;
    for (; pushed < (std::size_t{1} << 28) && !last; ++pushed)
        if (auto r = v->push_back(pushed); r.is_err()) last = r.unwrap_err_unchecked();
    assert(last == std::errc::no_space_on_device);
    // Every element it accepted is in place, and it is still usable
    for (std::size_t i = 0; i + 1 < pushed; ++i) assert((*v)[i] == i);
    assert(v->flush().is_ok());
    v->clear();
    assert(v->push_back(1).is_ok());
    std::cout << "      (ENOSPC after " << pushed - 1 << " elements)\n";
    std::filesystem::remove(file);
    pass("mapped_vector: a full disk is ENOSPC from push_back, and nothing touched an unbacked page");
}

} // namespace

int main()
{
    const char* tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/tsm_mmap_XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        std::perror("mkdtemp");
        return 1;
    }
    dir = buf.data();

    test_region_anonymous_grows_in_place();
    test_region_grow_or_move_keeps_contents();
    test_region_file_round_trip();
    test_region_file_moves_past_its_reservation();
    test_resource_object_pool_and_pmr_vector();
    test_vector_round_trip_past_the_reservation();
    test_vector_growth_inside_the_reservation_never_moves();
    test_vector_survives_a_kill_after_flush();
    test_vector_refuses_bad_headers();
    test_pool_persists_and_grows();
    test_pool_refuses_a_corrupt_state();
    test_full_disk_is_an_error_not_sigbus();

    std::filesystem::remove_all(dir);
    std::cout << "\nmmap: all " << tests_run << " tests passed (page size " << mapped_region::page_size() << ")\n";
    return 0;
}
