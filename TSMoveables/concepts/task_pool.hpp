//
//  concepts/task_pool.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The concept twin of interfaces/task_pool.hpp: "somewhere to submit work" as
//  a compile-time constraint instead of a vtable.
//
//  The interface stays, and stays the right tool whenever a pool is held by
//  pointer and chosen at run time: task_group and task_graph keep a
//  task_pool*, and a benchmark swaps pools behind one. What the concept adds is
//  the other case, which is most calls: the caller has the concrete pool in
//  hand, so the algorithm can be a template over it. parallel_for and friends
//  take any `Pool&` that models `task_pool_like`; given a concrete pool they
//  call it directly (every pool here is `final`, so those calls devirtualise
//  and inline), given a `task_pool&` they go through the vtable as before. The
//  call site picks the dispatch, and the same algorithm serves both.
//
//      work_stealing_task_pool pool;
//      parallel_for(pool, 0, n, body);                          // static: direct calls
//      parallel_for(static_cast<task_pool&>(pool), 0, n, body); // dynamic: virtual calls
//
//  The pieces, as in our other libraries' interface twins:
//
//    task_pool_like<P>       the members of task_pool as a concept, with their
//                            exact types, const and noexcept. The interface
//                            satisfies it (asserted at the bottom), so a member
//                            changed on one side and not the other fails here.
//    task_pool_adapter<Impl> task_pool implemented by forwarding to a model, for
//                            when something that is only task_pool_like has to
//                            be handed to code that holds a task_pool*. Holds the
//                            model by value, or binds to one when Impl is a
//                            reference (a non-owning view).
//    is_task_pool_like_v<P>  the same test as a C++17 trait.
//    TS_MOVEABLES_TASK_POOL  for writing one template that is concept-
//                            constrained under C++20 and still compiles under
//                            C++17: `template <TS_MOVEABLES_TASK_POOL Pool>`
//                            is `template <task_pool_like Pool>` with concepts
//                            and `template <typename Pool>` without, where the
//                            algorithm checks is_task_pool_like_v in a
//                            static_assert instead. ts-moveables still builds
//                            as C++17, so nothing that compiled before stops
//                            compiling.
//
//  The concept is syntactic only. The semantics - submit from any thread
//  (except dispatch_task_pool's single-dispatcher contract), wait_idle blocks
//  until everything submitted has run, worker_count is the parallelism the
//  algorithms may assume - are the interface's contract.
//

#ifndef concepts_task_pool_hpp
#define concepts_task_pool_hpp

#include <cstddef>
#include <type_traits>
#include <utility>

#include "../interfaces/task_pool.hpp"

// The language feature comes from the compiler, the library one from <version>
#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif
#if defined(__cpp_concepts) && __cpp_concepts >= 201907L && defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L
#define TS_MOVEABLES_HAS_CONCEPTS 1
#else
#define TS_MOVEABLES_HAS_CONCEPTS 0
#endif

#if TS_MOVEABLES_HAS_CONCEPTS
#include <concepts>
#endif

namespace snicholls
{
    namespace detail
    {
        template <typename P, typename = void>
        struct is_task_pool_like : std::false_type {};

        template <typename P>
        struct is_task_pool_like<P, std::void_t<
            decltype(std::declval<P&>().submit(std::declval<task_pool::task>())),
            decltype(std::declval<P&>().wait_idle()),
            decltype(std::declval<const P&>().worker_count())>>
            : std::bool_constant<
                  std::is_same_v<decltype(std::declval<P&>().submit(std::declval<task_pool::task>())), void> &&
                  std::is_same_v<decltype(std::declval<P&>().wait_idle()), void> &&
                  std::is_same_v<decltype(std::declval<const P&>().worker_count()), std::size_t> &&
                  noexcept(std::declval<const P&>().worker_count())> {};
    } // namespace detail

    // Whether F can be submitted as a task: called with no arguments (any
    // result is discarded, as std::function<void()> does), and movable into the
    // pool. The C++17 spelling of pool_task.
    template <typename F>
    inline constexpr bool is_pool_task_v =
        std::is_invocable_v<std::decay_t<F>&> && std::is_move_constructible_v<std::decay_t<F>> &&
        std::is_constructible_v<std::decay_t<F>, F>;

    // Whether P has task_pool's members: the C++17 spelling of task_pool_like.
    template <typename P>
    inline constexpr bool is_task_pool_like_v = detail::is_task_pool_like<std::remove_cv_t<P>>::value;

#if TS_MOVEABLES_HAS_CONCEPTS
    // The members of task_pool as a concept: submit a task (any std::function
    // <void()>, so a lambda converts), wait until idle, report the worker
    // count without throwing.
    template <typename P>
    concept task_pool_like =
        requires(P& p, task_pool::task t) {
            { p.submit(std::move(t)) } -> std::same_as<void>;
        } &&
        requires(P& p) {
            { p.wait_idle() } -> std::same_as<void>;
        } &&
        requires(const P& p) {
            { p.worker_count() } noexcept -> std::same_as<std::size_t>;
        };

#define TS_MOVEABLES_TASK_POOL ::snicholls::task_pool_like

    // A callable a pool can take as it is, with no std::function around it.
    // A concrete pool that can store one more cheaply than a std::function
    // (inline_task_pool runs it; work_stealing_task_pool allocates one node
    // holding it) has a `template <pool_task F> void submit(F&&)` beside its
    // submit(task) override: called on the concrete pool, the template is
    // chosen; called through task_pool&, the override is, as before.
    template <typename F>
    concept pool_task = std::invocable<std::decay_t<F>&> && std::move_constructible<std::decay_t<F>> &&
                        std::constructible_from<std::decay_t<F>, F>;
#define TS_MOVEABLES_POOL_TASK_TEMPLATE(F) template <::snicholls::pool_task F>
#else
#define TS_MOVEABLES_TASK_POOL typename
#define TS_MOVEABLES_POOL_TASK_TEMPLATE(F) \
    template <typename F, std::enable_if_t<::snicholls::is_pool_task_v<F>, int> = 0>
#endif

    // task_pool implemented by forwarding to a task_pool_like model. Impl is
    // the model, held by value, or a reference to one (a view that binds to it).
    template <typename Impl>
    class task_pool_adapter final : public task_pool
    {
        static_assert(is_task_pool_like_v<std::remove_reference_t<Impl>>,
                      "snicholls::task_pool_adapter: Impl does not have task_pool's members "
                      "(submit(task), wait_idle(), worker_count() const noexcept)");

    public:
        using implementation_type = std::remove_reference_t<Impl>;

        template <typename... Args,
                  std::enable_if_t<std::is_constructible_v<Impl, Args...>, int> = 0>
        explicit task_pool_adapter(Args&&... args) noexcept(std::is_nothrow_constructible_v<Impl, Args...>)
            : impl_(std::forward<Args>(args)...)
        {
        }

        implementation_type& implementation() noexcept { return impl_; }
        const implementation_type& implementation() const noexcept { return impl_; }

        void submit(task t) override { impl_.submit(std::move(t)); }
        void wait_idle() override { impl_.wait_idle(); }
        std::size_t worker_count() const noexcept override { return impl_.worker_count(); }

    private:
        Impl impl_;
    };

} // namespace snicholls

// The interface satisfies its own concept: a member changed on one side only fails here.
static_assert(snicholls::is_task_pool_like_v<snicholls::task_pool>,
              "snicholls::task_pool no longer matches is_task_pool_like_v in concepts/task_pool.hpp");
#if TS_MOVEABLES_HAS_CONCEPTS
static_assert(snicholls::task_pool_like<snicholls::task_pool>,
              "snicholls::task_pool no longer matches task_pool_like in concepts/task_pool.hpp");
static_assert(snicholls::pool_task<snicholls::task_pool::task> && !snicholls::pool_task<int>);
#endif
static_assert(snicholls::is_pool_task_v<snicholls::task_pool::task> && !snicholls::is_pool_task_v<int>);

#endif /* concepts_task_pool_hpp */
