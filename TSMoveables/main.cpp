//
//  main.cpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 5/4/2024.
//
//  Copyright 2024 Saxon Herschel Nicholls
//
//  Thread Safe Moveables

#include <iostream>
#include <thread>
#include <vector>
#include <utility>

#include "ts_moveables.hpp"

// We can copy and move the atomics
struct AtomicObject
{
    snicholls::moveable_atomic_int         a;
    snicholls::moveable_atomic_bool        b;
    snicholls::moveable_atomic_uint64_t    c;

    AtomicObject(   snicholls::moveable_atomic_int         mya,
                    snicholls::moveable_atomic_bool        myb,
                    snicholls::moveable_atomic_uint64_t    myc ) :
                    a{mya}, b{myb}, c{myc} {};

    AtomicObject(const AtomicObject&)   = default;
    AtomicObject( AtomicObject&& )      = default;
};

void AtomicMoveableExample()
{
    AtomicObject o1{ 42, true, 314159265360 };      // Construct
    AtomicObject o2(o1);                            // Copy
    AtomicObject o3( std::move(o1) );               // Move

    std::cout << "AtomicMoveableExample: o3.a = " << o3.a.get() << "\n";
}

// Thread safe bank account that we can move around - because the mutex,
// condition variable and once flag are all moveable, the compiler generates
// correct move operations for us and the account can live in a std::vector.
struct Account
{
    snicholls::moveable_mutex<>                 m;
    snicholls::moveable_condition_variable<>    funds_arrived;
    snicholls::moveable_once_flag               opened;
    long long                                   balance{0};

    void deposit(long long amount)
    {
        opened.call_once([]{ std::cout << "First deposit - account opened\n"; });
        {
            std::lock_guard< snicholls::moveable_mutex<> > lock(m);
            balance += amount;
        }
        funds_arrived.notify_all();
    }

    long long read_balance()
    {
        std::lock_guard< snicholls::moveable_mutex<> > lock(m);
        return balance;
    }
};

void MoveableMutexExample()
{
    // The classic problem: objects that own a mutex cannot normally live in a
    // vector, because growth requires moving them. Now they can.
    std::vector<Account> accounts;
    for (int i = 0; i < 8; ++i) {           // Forces several reallocations
        accounts.emplace_back();
        accounts.back().deposit(100 * (i + 1));
    }

    std::thread t1{ [&]{ accounts[0].deposit(50); } };
    std::thread t2{ [&]{ accounts[0].deposit(25); } };
    t1.join();
    t2.join();

    std::cout << "MoveableMutexExample: accounts[0] balance = " << accounts[0].read_balance() << "\n";
}

void SemaphoreLatchBarrierExample()
{
    snicholls::moveable_semaphore sem{2};   // At most two workers in the critical region
    snicholls::moveable_latch     done{4};
    snicholls::moveable_atomic_int in_flight{0};

    // All of these are moveable while quiescent
    snicholls::moveable_semaphore sem2( std::move(sem) );
    snicholls::moveable_latch     done2( std::move(done) );

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&]{
            sem2.acquire();
            ++in_flight;
            // ... do work ...
            --in_flight;
            sem2.release();
            done2.count_down();
        });
    }
    done2.wait();
    for (auto& w : workers) w.join();

    std::cout << "SemaphoreLatchBarrierExample: all workers finished\n";
}

int main()
{
    AtomicMoveableExample();
    MoveableMutexExample();
    SemaphoreLatchBarrierExample();

    return 0;
}
