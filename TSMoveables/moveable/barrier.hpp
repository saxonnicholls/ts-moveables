//
//  moveable_barrier.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_barrier_hpp
#define moveable_barrier_hpp

#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace snicholls
{
    struct null_barrier_completion {
        void operator()() const noexcept {}
    };

    // Moveable barrier
    //
    // std::barrier is immovable and its phase state cannot be observed, so a
    // wrapper could never preserve its state across a move. This is a portable
    // reusable barrier built on mutex + condition_variable whose entire state
    // lives under the mutex, making the move guarantee exact: a move transfers
    // the configuration (expected count, pending drops, phase number and the
    // completion function) if and only if the barrier is quiescent - no thread
    // waiting and no arrivals recorded in the current phase - on either side;
    // otherwise it throws std::runtime_error.
    //
    // The completion function runs on the last arriving thread while the
    // barrier's internal lock is held; it must not call back into the barrier
    // (as with std::barrier, where that is undefined behaviour) and should not
    // throw.
    template <typename CompletionFunction = null_barrier_completion>
    struct moveable_barrier {

        using completion_function_type = CompletionFunction;

    private:
        mutable std::mutex m;
        std::condition_variable cv;
        std::ptrdiff_t expected{0};       // arrivals required to complete the current phase
        std::ptrdiff_t next_expected{0};  // arrivals required for subsequent phases (reduced by arrive_and_drop)
        std::ptrdiff_t arrived{0};
        unsigned long long phase_count{0};
        int waiters{0};
        CompletionFunction completion;

        static void ensure_quiescent(int waiters_, std::ptrdiff_t arrived_) {
            if (waiters_ != 0 || arrived_ != 0)
                throw std::runtime_error("moveable_barrier: moving while threads are arriving or waiting");
        }

        // Requires the lock to be held
        void complete_phase() {
            completion();
            arrived = 0;
            expected = next_expected;
            ++phase_count;
            cv.notify_all();
        }

        // Move-construction helper: the lock on other.m is held by the caller for
        // the duration of this constructor. The quiescence check runs inside the
        // first mem-initializer, before completion is moved, so a throwing move
        // leaves the source fully intact.
        moveable_barrier(moveable_barrier& other, const std::lock_guard<std::mutex>&)
            : expected((ensure_quiescent(other.waiters, other.arrived), other.expected)),
              next_expected(other.next_expected),
              phase_count(other.phase_count),
              completion(std::move(other.completion)) {}

    public:
        explicit moveable_barrier(std::ptrdiff_t expected_count = 0, CompletionFunction f = CompletionFunction())
            : expected(expected_count), next_expected(expected_count), completion(std::move(f)) {}

        moveable_barrier(const moveable_barrier&) = delete;
        moveable_barrier& operator=(const moveable_barrier&) = delete;

        moveable_barrier(moveable_barrier&& other)
            : moveable_barrier(other, std::lock_guard<std::mutex>(other.m)) {}

        moveable_barrier& operator=(moveable_barrier&& other) {
            if (this != &other) {
                std::scoped_lock lock(m, other.m);
                ensure_quiescent(waiters, arrived);
                ensure_quiescent(other.waiters, other.arrived);
                expected = other.expected;
                next_expected = other.next_expected;
                phase_count = other.phase_count;
                completion = std::move(other.completion);
            }
            return *this;
        }

        void arrive_and_wait() {
            std::unique_lock<std::mutex> lock(m);
            ++arrived;
            if (arrived == expected) {
                complete_phase();
                return;
            }
            const auto my_phase = phase_count;
            ++waiters;
            cv.wait(lock, [this, my_phase] { return phase_count != my_phase; });
            --waiters;
        }

        // Arrive at the current phase without waiting, and reduce the expected
        // count for all subsequent phases by one
        void arrive_and_drop() {
            std::lock_guard<std::mutex> lock(m);
            if (next_expected == 0)
                throw std::logic_error("moveable_barrier: arrive_and_drop below zero");
            --next_expected;
            ++arrived;
            if (arrived == expected)
                complete_phase();
        }

        // Completed phase count - a snapshot, for testing and diagnostics
        unsigned long long phase() const {
            std::lock_guard<std::mutex> lock(m);
            return phase_count;
        }
    };

} // namespace snicholls

#endif /* moveable_barrier_hpp */
