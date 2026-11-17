//
//  global.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - one-off, process-wide initialisers (an SDK's
//  InitAPI, curl_global_init, OpenSSL, gRPC, a connection pool) as RAII
//  singletons with a defined start and a defined, ordered shutdown:
//
//      struct aws_sdk {
//          static constexpr std::string_view name = "aws_sdk";
//          aws_sdk()  { Aws::InitAPI(options); }       // may throw: the next get() retries
//          ~aws_sdk() { Aws::ShutdownAPI(options); }   // must not throw
//          Aws::SDKOptions options;
//      };
//      snicholls::global<aws_sdk>::get();              // any thread, any number of times: constructed once
//      snicholls::shutdown_globals();                  // reverse order of completion
//
//  ------------------------------------------------------------------ the rules
//
//    - Idempotent and thread-safe. Once initialised, get() is one acquire
//      load. Concurrent first callers wait for one constructor. A constructor
//      that throws leaves the global uninitialised, the exception propagates,
//      and the next get() tries again.
//    - Dependencies: call global<dep>::get() inside the constructor. dep
//      completes first, so it shuts down last. A cycle on one thread (a needs
//      b needs a) throws global_init_error instead of deadlocking. A cycle
//      across threads - each constructor waiting on a global the other thread
//      is constructing - is not detected; keep constructors to their own
//      dependencies.
//    - Shutdown is explicit: shutdown_globals() destroys every initialised
//      global in reverse order of completion, before static destruction and
//      never from atexit. A global_scope does it at the end of main(). Call
//      it once nothing still uses the globals: a reference from an earlier
//      get() dangles afterwards.
//    - After shutdown, get() throws global_init_error unless T declares
//      `static constexpr bool reinitialisable = true` (most SDKs cannot be
//      started twice).
//    - Storage is leaked on purpose: no static destructor ever runs, so
//      nothing depends on the order of static destruction across files.
//
//  T must be default-constructible, nothrow-destructible, and have a static
//  `name` convertible to std::string_view (shown by active_globals() and in
//  errors).
//
//  Moved here from an earlier application framework, whose copy was the best
//  of several; the behaviour is the same, the debug log lines are not
//  (subscribe with set_global_observer).
//
//  C++17, standard library only.
//

#ifndef ts_moveables_process_global_hpp
#define ts_moveables_process_global_hpp

#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace snicholls {

// Thrown by global<T>::get() on a cycle, or after shutdown when T is not reinitialisable
class global_init_error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// What a global observer is told
enum class global_event { initialising, initialised, shutting_down };

// Called for every global as it starts and stops: wire it to a logger. A plain function pointer, so it costs
// nothing when unset and needs no allocation; it must not throw.
using global_observer = void (*)(global_event, std::string_view name) noexcept;

namespace detail::globals {

template <class T, class = void>
struct has_name : std::false_type {};
template <class T>
struct has_name<T, std::void_t<decltype(T::name)>>
    : std::bool_constant<std::is_convertible_v<decltype(T::name), std::string_view>> {};

template <class T, class = void>
struct reinitialisable : std::false_type {};
template <class T>
struct reinitialisable<T, std::void_t<decltype(T::reinitialisable)>> : std::bool_constant<T::reinitialisable> {};

inline std::atomic<global_observer>& observer() noexcept
{
    static std::atomic<global_observer> o{nullptr};   // constant-initialised, so safe from any static initialiser
    return o;
}

inline void notify(global_event e, std::string_view name) noexcept
{
    if (global_observer o = observer().load(std::memory_order_acquire)) o(e, name);
}

class registry
{
public:
    struct record
    {
        std::string_view name;
        void (*destroy)() noexcept;
    };

    static registry& instance() noexcept
    {
        static registry* const r = new registry;   // leaked on purpose
        return *r;
    }

    void completed(std::string_view name, void (*destroy)() noexcept)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back({name, destroy});
    }

    // Destroys in reverse order of completion; the names in the order they were shut down
    std::vector<std::string> shutdown_all() noexcept
    {
        std::vector<std::string> names;
        for (;;) {
            record r;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (records_.empty()) break;
                r = records_.back();
                records_.pop_back();
            }
            try {
                names.emplace_back(r.name);
            } catch (...) {
            }
            notify(global_event::shutting_down, r.name);
            r.destroy();   // never under the registry lock, so a destructor may use other globals
        }
        return names;
    }

    std::vector<std::string> active() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        names.reserve(records_.size());
        for (const record& r : records_) names.emplace_back(r.name);
        return names;
    }

private:
    mutable std::mutex mutex_;
    std::vector<record> records_;
};

} // namespace detail::globals

template <class T>
class global
{
    static_assert(std::is_default_constructible_v<T>, "snicholls::global<T>: T must be default-constructible");
    static_assert(std::is_nothrow_destructible_v<T>, "snicholls::global<T>: T's destructor must not throw");
    static_assert(detail::globals::has_name<T>::value,
                  "snicholls::global<T>: T needs `static constexpr std::string_view name = \"...\";`");

public:
    global() = delete;

    // The instance, constructed on first use
    static T& get()
    {
        state& s = storage();
        if (T* p = s.instance.load(std::memory_order_acquire)) return *p;
        return slow_get(s);
    }

    [[nodiscard]] static bool initialised() noexcept
    {
        return storage().instance.load(std::memory_order_acquire) != nullptr;
    }

    [[nodiscard]] static bool shut_down() noexcept { return storage().shut_down.load(std::memory_order_acquire); }

private:
    struct state
    {
        std::mutex mutex;
        std::atomic<T*> instance{nullptr};
        std::atomic<std::thread::id> initialising{};
        std::atomic<bool> shut_down{false};
        alignas(T) unsigned char buffer[sizeof(T)];
    };

    static state& storage() noexcept
    {
        static state* const s = new state;   // leaked on purpose
        return *s;
    }

    static T& slow_get(state& s)
    {
        if (s.initialising.load(std::memory_order_acquire) == std::this_thread::get_id())
            throw global_init_error("snicholls::global: initialisation cycle through " + std::string(T::name));
        const std::lock_guard<std::mutex> lock(s.mutex);
        if (T* p = s.instance.load(std::memory_order_acquire)) return *p;
        if (s.shut_down.load(std::memory_order_acquire) && !detail::globals::reinitialisable<T>::value)
            throw global_init_error("snicholls::global: " + std::string(T::name) +
                                    " was shut down and is not reinitialisable");
        s.initialising.store(std::this_thread::get_id(), std::memory_order_release);
        struct reset_owner
        {
            state& s;
            ~reset_owner() { s.initialising.store(std::thread::id{}, std::memory_order_release); }
        } guard{s};
        detail::globals::notify(global_event::initialising, T::name);
        T* p = ::new (static_cast<void*>(s.buffer)) T();   // throws: nothing recorded, the next get() retries
        s.shut_down.store(false, std::memory_order_release);
        s.instance.store(p, std::memory_order_release);
        detail::globals::registry::instance().completed(T::name, &destroy);
        detail::globals::notify(global_event::initialised, T::name);
        return *p;
    }

    static void destroy() noexcept
    {
        state& s = storage();
        const std::lock_guard<std::mutex> lock(s.mutex);
        if (T* p = s.instance.exchange(nullptr, std::memory_order_acq_rel)) p->~T();
        s.shut_down.store(true, std::memory_order_release);
    }
};

// Shut down every initialised global in reverse order of completion; their names in shutdown order
inline std::vector<std::string> shutdown_globals() noexcept { return detail::globals::registry::instance().shutdown_all(); }

// The initialised globals, in order of completion
[[nodiscard]] inline std::vector<std::string> active_globals() { return detail::globals::registry::instance().active(); }

// Set (or clear, with nullptr) the observer told of every start and stop; returns the previous one
inline global_observer set_global_observer(global_observer o) noexcept
{
    return detail::globals::observer().exchange(o, std::memory_order_acq_rel);
}

// shutdown_globals() at the end of a scope: put one at the top of main()
class global_scope
{
public:
    global_scope() = default;
    global_scope(const global_scope&) = delete;
    global_scope& operator=(const global_scope&) = delete;
    ~global_scope() { shutdown_globals(); }
};

} // namespace snicholls

#endif // ts_moveables_process_global_hpp
