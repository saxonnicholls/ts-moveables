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

# Head-to-head vs moodycamel. Fetches the third-party headers first (gitignored).
# Warning flags relaxed here because the third-party headers are not ours to lint.
benchmarks/third_party/concurrentqueue.h:
	bash scripts/fetch_bench_deps.sh

build/bench_compare: benchmarks/bench.cpp $(HEADERS) benchmarks/third_party/concurrentqueue.h | build
	$(CXX) -std=$(STD) -O3 -DNDEBUG -DTS_BENCH_COMPARE -Ibenchmarks/third_party -pthread benchmarks/bench.cpp -o $@ $(LDLIBS)

build/signal_slot_demo: demos/signal_slot_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/signal_slot_demo.cpp -o $@ $(LDLIBS)

build/capture_replay_demo: demos/capture_replay_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/capture_replay_demo.cpp -o $@ $(LDLIBS)

build/pcap_replay_demo: demos/pcap_replay_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/pcap_replay_demo.cpp -o $@ $(LDLIBS)

build/taskflow_style_demo: demos/taskflow_style_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/taskflow_style_demo.cpp -o $@ $(LDLIBS)

build/time_master_demo: demos/time_master_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/time_master_demo.cpp -o $@ $(LDLIBS)

build/http_server_demo: demos/http_server_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/http_server_demo.cpp -o $@ $(LDLIBS)

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

bench-compare: build/bench_compare
	./build/bench_compare

demo-signals: build/signal_slot_demo
	./build/signal_slot_demo

demo-capture: build/capture_replay_demo
	./build/capture_replay_demo

demo-pcap: build/pcap_replay_demo
	./build/pcap_replay_demo

demo-taskflow: build/taskflow_style_demo
	./build/taskflow_style_demo

demo-timemaster: build/time_master_demo
	./build/time_master_demo

demo-http: build/http_server_demo
	./build/http_server_demo

# One self-contained file per entry header, for drop-in use
amalgamate:
	python3 scripts/amalgamate.py http_server.hpp
	python3 scripts/amalgamate.py ts_moveables.hpp

# The amalgamated header must always still compile - and run - on its own,
# so the single-file drop-in claim can never quietly drift
check-amalgamate: amalgamate | build
	printf '#include "ts_http_server.hpp"\nint main(){ snicholls::http::server s; s.get("/", [](const auto&, auto r){ r.send(200, "text/plain", "ok"); }); return s.listen("127.0.0.1", 0) ? 0 : 1; }\n' > build/amalgam_check.cpp
	$(CXX) -std=$(STD) -Wall -Wextra -O2 -pthread -Isingle_include build/amalgam_check.cpp -o build/amalgam_check
	./build/amalgam_check && echo "single-header drop-in: builds and runs"

clean:
	rm -rf build

.PHONY: all test tsan asan demo bench demo-signals demo-capture demo-pcap demo-taskflow demo-timemaster demo-http amalgamate check-amalgamate clean
