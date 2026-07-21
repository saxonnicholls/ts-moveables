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

#endif /* ts_moveables_test_helpers_hpp */
