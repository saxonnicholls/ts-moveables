//
//  interfaces/task_pool.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Somewhere to submit work - the interface the thread pools share.
//
//  It is here on its own because the point of the pools was always
//  *comparison*: one shared queue versus sharded versus work-stealing, weighed
//  through a common surface. Code that only needs "somewhere to run this"
//  should depend on that surface, not on five implementations.
//

#ifndef interfaces_task_pool_hpp
#define interfaces_task_pool_hpp

#include <cstddef>
#include <functional>

namespace snicholls
{
    struct task_pool {
        using task = std::function<void()>;

        virtual ~task_pool() = default;

        virtual void submit(task t) = 0;                    // enqueue work
        virtual void wait_idle() = 0;                       // block until all submitted work has run
        virtual std::size_t worker_count() const noexcept = 0;
    };
} // namespace snicholls

#endif /* interfaces_task_pool_hpp */
