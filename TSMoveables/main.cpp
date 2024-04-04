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
#include "moveable_atomic.hpp"

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

int main()
{
    AtomicObject o1{ 42, true, 314159265360 };      // Construct
    AtomicObject o2(o1);                            // Copy
    AtomicObject o3( std::move(o1) );               // Move

    return 0;
}
