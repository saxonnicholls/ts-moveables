//
//  tests_moveability.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the library's own thesis, asserted against itself
//
//  The pitch is that immovability is imposed by the standard, spreads virally
//  through every type that contains an immovable member, and that this library
//  cures it. A component here that is accidentally immovable falsifies that in
//  the most embarrassing possible way, and it is easy to do by accident: one
//  raw std::mutex member, or a user-declared copy constructor (which suppresses
//  the implicit move), and the type is stuck - with nothing to warn you.
//
//  So the claim is a compile-time gate rather than a paragraph in a README.
//  These are static_asserts: if one fails the build stops, and it stops in the
//  file whose job is to say why.
//
//  There is exactly one deliberate exception, asserted as such at the bottom.
//  The rule that separates it from the rest:
//
//    moveable      the type was only immovable because of what it CONTAINS.
//                  That is the viral case, and it is what this library exists
//                  to fix.
//    immovable     the type's ADDRESS is part of its contract - something
//                  outside it holds a pointer and will keep using it. Moving is
//                  not an ergonomic problem to solve, it is a bug to prevent,
//                  so the move is deleted explicitly and the reason recorded.
//
//  Ownership of an OS handle (a file descriptor, a socket) is a third case and
//  deliberately not this library's business. An fd is an int - already copyable
//  AND movable - so it is not an instance of the problem here at all; its
//  hazard is the opposite one, that copying gives two names for one kernel
//  object. What that needs is a unique owner, which is move-only by
//  construction, not a moveable wrapper around something immovable.
//

#include "test_helpers.hpp"

#include "../TSMoveables/ts_moveables.hpp"
#include "../TSMoveables/http/server.hpp"
#include "../TSMoveables/http/websocket.hpp"
#include "../TSMoveables/http/websocket_client.hpp"
#include "../TSMoveables/http/ws_broadcast_hub.hpp"
#include "../TSMoveables/logging/logger.hpp"
#include "../TSMoveables/http/websocket_deflate.hpp"

#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace snicholls;

#define MOVEABLE(...)                                                          \
    static_assert(std::is_move_constructible_v<__VA_ARGS__>,                   \
                  #__VA_ARGS__ " must be move constructible");                 \
    static_assert(std::is_move_assignable_v<__VA_ARGS__>,                      \
                  #__VA_ARGS__ " must be move assignable")

// ------------------------------------------------- the moveable primitives
// The whole point of the library. Every immovable synchronisation primitive in
// the standard library has a wrapper here, including every mutex flavour and
// both condition_variable flavours - that is what "complete" means for this
// component, and this is where it is checked.
MOVEABLE(moveable_mutex<>);
MOVEABLE(moveable_recursive_mutex);
MOVEABLE(moveable_shared_mutex);
MOVEABLE(moveable_timed_mutex);
MOVEABLE(moveable_recursive_timed_mutex);
MOVEABLE(moveable_shared_timed_mutex);
MOVEABLE(moveable_condition_variable<>);
MOVEABLE(moveable_condition_variable_any);
MOVEABLE(moveable_atomic<int>);
MOVEABLE(moveable_atomic_flag);
MOVEABLE(moveable_latch);
MOVEABLE(moveable_barrier<>);
MOVEABLE(moveable_semaphore);
MOVEABLE(moveable_once_flag);
MOVEABLE(moveable_spin_lock);
MOVEABLE(moveable_signal<int>);
MOVEABLE(scoped_connection);

// ------------------------------------------------------- the containers
MOVEABLE(synchronized<std::vector<int>>);
MOVEABLE(synchronized_waitable<std::vector<int>>);
MOVEABLE(synchronized_type_map);
MOVEABLE(circular_buffer<int>);
MOVEABLE(mpmc_queue<int>);
MOVEABLE(disruptor<int>);
MOVEABLE(intern_pool<std::string>);

// ------------------------------------------------------------- the pools
MOVEABLE(mutex_task_pool);
MOVEABLE(sharded_task_pool);
MOVEABLE(dispatch_task_pool);
MOVEABLE(mpmc_task_pool);
MOVEABLE(work_stealing_task_pool);
MOVEABLE(task_graph);

// ------------------------------------------------ the reactor and its users
//
// Guarded on the same macros the components themselves are guarded on. The
// reactor and everything above it is POSIX-only and compiles to nothing on
// Windows, so asserting on the types unconditionally is not a stricter test -
// it is a test that cannot build, which is how the first version of this file
// broke both Windows jobs while every POSIX one stayed green.
#if SNICHOLLS_HAS_EVENT_LOOP
MOVEABLE(event_loop);
#endif
#if SNICHOLLS_HAS_TIME_MASTER
MOVEABLE(time_master);
#endif
#if SNICHOLLS_HAS_HTTP_SERVER
MOVEABLE(http::server);
MOVEABLE(http::responder);
MOVEABLE(http::response_stream);
MOVEABLE(http::request);
MOVEABLE(http::response);
#endif
#if SNICHOLLS_HAS_WEBSOCKET
MOVEABLE(http::websocket);
#endif
#if SNICHOLLS_HAS_WEBSOCKET_CLIENT
MOVEABLE(http::websocket_client);      // its own macro, not the parent's
#endif
#if SNICHOLLS_HAS_WS_BROADCAST_HUB
MOVEABLE(http::ws_broadcast_hub);
#endif
MOVEABLE(log::logger);

// ------------------------------------------------- deliberately immovable
//
// Two types, two different reasons, both about ADDRESS IDENTITY rather than
// about what they contain - so neither is a gap in the thesis above.

// A Chase-Lev deque is stolen from concurrently: thieves hold a pointer to it,
// so a live one that moved would strand them. work_stealing_task_pool keeps
// them in a std::deque for stable addresses and constructs in place.
static_assert(!std::is_move_constructible_v<work_stealing_deque<int>>,
              "work_stealing_deque must stay immovable - thieves hold its address");

// permessage_deflate owns two z_streams, and zlib keeps internal pointers into
// its own stream struct, so moving one corrupts it. Held behind a unique_ptr
// instead, which is the same answer as a stable address. Guarded because the
// type only exists where zlib does.
#if SNICHOLLS_HAS_WS_DEFLATE
static_assert(!std::is_move_constructible_v<http::permessage_deflate>,
              "permessage_deflate must stay immovable - z_stream self-references");
#endif

// task_group is scope-bound like a lock_guard: outstanding tasks point at it,
// and moving one is not a use case, it is a bug that would otherwise compile.
static_assert(!std::is_move_constructible_v<task_group>,
              "task_group must stay immovable - running tasks hold its state");

} // namespace

void run_moveability_tests()
{
    // Everything above is checked at compile time; reaching here means it all
    // held. The runtime half is a single smoke check that a moved-from handle
    // really does transfer rather than merely compile.
    moveable_mutex<> a;
    moveable_mutex<> b(std::move(a));
    std::lock_guard<moveable_mutex<>> g(b);

    pass("moveability: every shipped type is moveable, or immovable on purpose");
}
