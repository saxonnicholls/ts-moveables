//
//  test_helpers.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - shared unit test helpers
//
//  Deliberately lightweight: cassert only, no test framework.
//  Every test translation unit must include this header first, so that
//  NDEBUG is undefined before <cassert> is seen and asserts stay active
//  in optimised builds.
//

#ifndef ts_moveables_test_helpers_hpp
#define ts_moveables_test_helpers_hpp

#undef NDEBUG
#include <cassert>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

inline int tests_run = 0;

inline void pass(const char* name)
{
    ++tests_run;
    std::cout << "PASS  " << name << "\n";
}

// Spin until a condition is true - avoids sleep-based flakiness
template <typename Pred>
void spin_until(Pred pred)
{
    while (!pred())
        std::this_thread::yield();
}

// A note that has cost this repository four CI failures, so it lives here
// where every test file will see it:
//
//   A `constexpr` local used inside a lambda that has NO default capture mode
//   (`[a, b]` rather than `[&]` or `[&, b]`) is rejected by MSVC with C3493 -
//   "cannot be implicitly captured because no default capture mode has been
//   specified" - even though GCC and Clang accept it, since a constant needs
//   no capture. The fix is `static constexpr`: static storage duration means
//   there is nothing to capture on any compiler. Prefer that to adding a
//   default capture mode, because it keeps the explicit capture list honest
//   about what the lambda actually touches.
//
// Bounded spin: returns false if the condition has not come true within the
// deadline. Use it (with assert) wherever a broken invariant would otherwise
// hang the suite instead of failing it. The limit is deliberately generous -
// it is a backstop, not a timing assertion.
template <typename Pred>
bool spin_until_for(Pred pred, std::chrono::milliseconds limit = std::chrono::milliseconds(30000))
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

template <typename F>
bool throws_runtime_error(F&& f)
{
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

template <typename F>
bool throws_logic(F&& f)
{
    try {
        f();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

#endif /* ts_moveables_test_helpers_hpp */
