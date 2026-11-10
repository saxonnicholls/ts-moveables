//
//  seqlock.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a sequence lock: one writer publishes a value,
//  any number of readers take consistent copies, and nobody waits on anybody
//
//      snicholls::seqlock<quote> top{quote{}};
//
//      top.write(q);                          // the one writer: never blocks
//      quote now = top.read();                // any reader: never blocks the writer
//      if (top.try_read(now)) { ... }         // or one attempt, no spinning
//      auto [value, version] = top.read_versioned();   // "has it changed since?"
//
//  The shape is "latest value": top of book, the latest portfolio, a risk
//  limit, a configuration. A reader writes NOTHING shared - it reads a
//  sequence number, copies the value, and reads the number again, retrying if
//  a write happened in between - so readers scale with their number, where
//  every reader of a mutex or shared_mutex writes the lock's cache line and
//  slows the others down. The writer bumps the number, writes, and bumps it
//  again: no reader can ever hold it up.
//
//  ------------------------------------------------------ why atomic words
//
//  The textbook seqlock copies the value with memcpy (or plain assignment)
//  while the writer may be writing it, relying on the retry to discard a torn
//  copy. In C++ that concurrent non-atomic access is a data race - undefined
//  behaviour, whatever the retry does afterwards - and ThreadSanitizer says
//  so. Here the value is held as an array of std::atomic<std::uint64_t>, the
//  construction Hans Boehm describes in "Can seqlocks get along with
//  programming language memory models?" (MSPC 2012). Every access is atomic,
//  so there is no race to excuse.
//
//  The orderings are on the atomics themselves, not in fences, because
//  ThreadSanitizer does not model std::atomic_thread_fence (the same reason
//  work_stealing_deque avoids them):
//
//    writer   seq <- s+1 (relaxed); each word <- release; seq <- s+2 (release)
//    reader   s0 <- seq (acquire); each word (acquire); s1 <- seq (relaxed)
//
//  A reader that sees any word of a write W also sees W's odd sequence number
//  in s1 (the word's release carries it), so it retries; a reader that sees an
//  even s0 sees every word of the write that published it. On x86-64 every one
//  of those loads and stores is a plain mov. On ARM64 they are ldar and stlr,
//  one per 8-byte word - which is why T should be small.
//
//  ----------------------------------------------------------- the rules
//
//    - T is trivially copyable (a torn copy is discarded, never used, but it
//      is built - harmless for plain data, undefined for anything with a
//      constructor) and default constructible (read() returns one).
//    - ONE writer at a time. Two threads calling write() concurrently corrupt
//      the sequence; serialise writers yourself if there are several.
//    - Keep T small: a struct of prices, not an order book. Every read copies
//      all of it, and every write it overlaps is a retry.
//    - Under continuous writes a reader can retry for a long time. try_read()
//      is one attempt, for a reader that must not spin.
//
//  Moveable: the state lives behind a heap core, as everywhere in this
//  library, so a quiescent handle moves; moving it while threads use it is
//  the caller's bug, as for every type here.
//

#ifndef ts_moveables_concurrent_seqlock_hpp
#define ts_moveables_concurrent_seqlock_hpp

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

#include "../utils/cpu_relax.hpp"

namespace snicholls
{
    template <typename T>
    class seqlock {
        static_assert(std::is_trivially_copyable<T>::value,
                      "snicholls::seqlock<T>: T must be trivially copyable (a torn copy is built, then discarded)");
        static_assert(std::is_default_constructible<T>::value,
                      "snicholls::seqlock<T>: T must be default constructible (read() returns one)");

        static constexpr std::size_t cache_line_size =
#if defined(__aarch64__) || defined(_M_ARM64)
            128;
#else
            64;
#endif
        static constexpr std::size_t word_count = (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

        // The sequence number and the words share one aligned block: a reader
        // needs both, and the writer writes both, so splitting them would cost
        // every reader a second cache miss for nothing
        struct alignas(cache_line_size) core {
            std::atomic<std::uint64_t> seq{0};
            std::atomic<std::uint64_t> words[word_count];

            explicit core(const T& initial) noexcept {
                std::uint64_t buf[word_count] = {};
                std::memcpy(buf, &initial, sizeof(T));
                for (std::size_t i = 0; i < word_count; ++i)
                    words[i].store(buf[i], std::memory_order_relaxed);
            }
        };

        std::unique_ptr<core> c_;

        // One attempt at a consistent copy: false if a write was in progress
        // or overlapped it. On success `out` and `version` are set.
        bool attempt(T& out, std::uint64_t& version) const noexcept {
            const std::uint64_t s0 = c_->seq.load(std::memory_order_acquire);
            if (s0 & 1u)
                return false;                       // a write is in progress
            std::uint64_t buf[word_count];
            for (std::size_t i = 0; i < word_count; ++i)
                buf[i] = c_->words[i].load(std::memory_order_acquire);
            // Ordered after the word loads by their acquire: if any of them saw
            // a newer write, this sees that write's odd number at least
            const std::uint64_t s1 = c_->seq.load(std::memory_order_relaxed);
            if (s0 != s1)
                return false;                       // a write overlapped the copy
            std::memcpy(&out, buf, sizeof(T));
            version = s0 / 2;
            return true;
        }

    public:
        explicit seqlock(const T& initial = T{}) : c_(std::make_unique<core>(initial)) {}

        seqlock(seqlock&&) noexcept = default;
        seqlock& operator=(seqlock&&) noexcept = default;

        // The one writer. Never blocks, never fails.
        void write(const T& value) noexcept {
            std::uint64_t buf[word_count] = {};
            std::memcpy(buf, &value, sizeof(T));
            const std::uint64_t s = c_->seq.load(std::memory_order_relaxed);   // only this thread writes it
            c_->seq.store(s + 1, std::memory_order_relaxed);                  // odd: in progress
            // Release on each word carries the odd number with it, so a reader
            // that sees any new word also sees that a write began
            for (std::size_t i = 0; i < word_count; ++i)
                c_->words[i].store(buf[i], std::memory_order_release);
            c_->seq.store(s + 2, std::memory_order_release);                  // even: published
        }

        // A consistent copy. Spins (politely) while writes overlap it; never
        // blocks the writer.
        T read() const noexcept {
            T out;
            std::uint64_t version = 0;
            while (!attempt(out, version))
                utils::cpu_relax();
            return out;
        }

        // One attempt, no spinning: true and `out` set if no write overlapped.
        bool try_read(T& out) const noexcept {
            std::uint64_t version = 0;
            return attempt(out, version);
        }

        // A consistent copy and the number of writes that preceded it, for
        // "has it changed since I last looked?" without comparing values.
        std::pair<T, std::uint64_t> read_versioned() const noexcept {
            std::pair<T, std::uint64_t> r{};
            while (!attempt(r.first, r.second))
                utils::cpu_relax();
            return r;
        }

        // Completed writes so far. Racy by nature: a hint, e.g. for a reader
        // polling for change before it pays for a copy.
        std::uint64_t version() const noexcept {
            return c_->seq.load(std::memory_order_acquire) / 2;
        }
    };

} // namespace snicholls

#endif /* ts_moveables_concurrent_seqlock_hpp */
