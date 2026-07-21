# TSMoveables - Thread Safe Moveables
#
# The library itself is header-only (TSMoveables/*.hpp).
# This Makefile builds the demo and the cassert-based unit tests.
#
#   make test    build and run the unit tests
#   make tsan    build and run the unit tests under ThreadSanitizer
#   make asan    build and run the unit tests under Address+UB Sanitizers
#   make demo    build and run the demo (same code as the Xcode target)
#   make clean
#
#   make test STD=c++17    any target can be built against a different standard

CXX      ?= c++
STD      ?= c++20
CXXFLAGS ?= -std=$(STD) -Wall -Wextra -pedantic -O2 -g

# Some targets (notably Clang on AArch64 Linux) lower std::atomic operations
# such as is_lock_free() to libatomic calls instead of inlining them
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LDLIBS += -latomic
endif

HEADERS  := $(wildcard TSMoveables/*.hpp) $(wildcard tests/*.hpp)
TEST_SRC := $(wildcard tests/*.cpp)

all: build/tests build/demo

build:
	mkdir -p build

build/tests: $(TEST_SRC) $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -pthread $(TEST_SRC) -o $@ $(LDLIBS)

build/tests_tsan: $(TEST_SRC) $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -pthread -fsanitize=thread $(TEST_SRC) -o $@ $(LDLIBS)

build/tests_asan: $(TEST_SRC) $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -pthread -fsanitize=address,undefined $(TEST_SRC) -o $@ $(LDLIBS)

build/demo: TSMoveables/main.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -pthread TSMoveables/main.cpp -o $@ $(LDLIBS)

build/bench: benchmarks/bench.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread benchmarks/bench.cpp -o $@ $(LDLIBS)

test: build/tests
	./build/tests

tsan: build/tests_tsan
	TSAN_OPTIONS="suppressions=tests/tsan.supp" ./build/tests_tsan

asan: build/tests_asan
	./build/tests_asan

demo: build/demo
	./build/demo

bench: build/bench
	./build/bench

clean:
	rm -rf build

.PHONY: all test tsan asan demo bench clean
