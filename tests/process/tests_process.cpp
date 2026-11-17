//
//  tests_process.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the process/ tests (signals, exit codes, .env, config, environment, health and the boot
//  gate), one program of their own: C++20 and POSIX, and they raise real signals and fork death tests, which the main
//  suite should not share a process with. `make test-process` runs it.
//

#include "mini_gtest.hpp"

int main() { return mini_gtest_main(); }
