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

HEADERS  := $(shell find TSMoveables -name '*.hpp') $(wildcard tests/*.hpp)
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

build/drone_fleet_demo: demos/drone_fleet_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    demos/drone_fleet_demo.cpp -o $@ $(LDLIBS)

demo-drones: build/drone_fleet_demo
	./build/drone_fleet_demo

build/replay_loop_demo: demos/replay_loop_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    demos/replay_loop_demo.cpp -o $@ $(LDLIBS)

demo-replay: build/replay_loop_demo
	./build/replay_loop_demo

build/http_server_demo: demos/http_server_demo.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/http_server_demo.cpp -o $@ $(LDLIBS)

# A static check for the one MSVC rule this repository keeps tripping over
check-msvc:
	python3 scripts/check_msvc_capture.py

test: build/tests
	./build/tests

# TLS is the one opt-in part of the library: it needs a third-party library, so
# it builds and runs separately and the core stays dependency-free.
#
# There are two backends and one test binary. Every test body runs against both,
# which is what keeps "the backend is a run-time choice" an observation rather
# than a claim. OpenSSL is required - the test client is built from it either
# way - and mbedTLS is optional: if its headers are not installed the second
# backend is compiled out and that half of the suite reports itself skipped,
# rather than breaking the build for anyone who only has OpenSSL.
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null || echo /usr)
MBEDTLS_PREFIX ?= $(shell brew --prefix mbedtls 2>/dev/null || echo /usr)

# build_info.h rather than ssl.h as the probe: it arrived in mbedTLS 3.0, so its
# presence doubles as the version check the header needs anyway
ifneq ($(wildcard $(MBEDTLS_PREFIX)/include/mbedtls/build_info.h),)
  MBEDTLS_CPPFLAGS := -DSNICHOLLS_TEST_MBEDTLS -I$(MBEDTLS_PREFIX)/include
  MBEDTLS_LDLIBS   := -L$(MBEDTLS_PREFIX)/lib -lmbedtls -lmbedx509 -lmbedcrypto
  MBEDTLS_FOUND    := 1
else
  MBEDTLS_FOUND    := 0
endif

# Optional locally, mandatory in CI, and the difference has to be enforced.
# A second backend that is quietly not compiled is not a second backend - the
# absence reads as a pass, which is how 537 lines of it went a whole release
# cycle without any automated system ever building them. CI runs this first.
check-mbedtls:
ifeq ($(MBEDTLS_FOUND),1)
	@echo "mbedTLS: found at $(MBEDTLS_PREFIX)"
else
	@echo "mbedTLS: NOT found at $(MBEDTLS_PREFIX)/include/mbedtls/build_info.h"
	@echo "  the mbedTLS backend would be compiled out and its half of the"
	@echo "  suite would report itself skipped - which is not a pass."
	@echo "  Linux: sudo apt-get install -y libmbedtls-dev   macOS: brew install mbedtls"
	@exit 1
endif

build/tests_tls: tests/tls/tests_tls.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -pthread -I$(OPENSSL_PREFIX)/include $(MBEDTLS_CPPFLAGS) \
	    tests/tls/tests_tls.cpp -o $@ \
	    -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto $(MBEDTLS_LDLIBS) $(LDLIBS)

test-tls: build/tests_tls
	@echo "backends compiled: openssl$(if $(filter 1,$(MBEDTLS_FOUND)), + mbedtls, ONLY - mbedTLS absent)"
	./build/tests_tls

# Autobahn|Testsuite - the external RFC 6455 grader. The echo server is ours;
# the fuzzing client comes from the suite (venv or Docker, see the script).
# The echo server is built with permessage-deflate wired in (zlib), so the
# Autobahn compression groups are graded rather than skipped
build/ws_echo_server: tests/autobahn/ws_echo_server.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O2 -pthread -DSNICHOLLS_AUTOBAHN_DEFLATE \
	    tests/autobahn/ws_echo_server.cpp -o $@ -lz $(LDLIBS)

autobahn: build/ws_echo_server
	./scripts/run_autobahn.sh

# h2spec - the external RFC 9113 / RFC 7541 grader. Same shape as Autobahn:
# the server under test is ours, the conformance client comes from the
# official image, and the runner distinguishes "could not run" from "failed"
build/h2_server: tests/h2spec/h2_server.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O2 -pthread \
	    tests/h2spec/h2_server.cpp -o $@ $(LDLIBS)

h2spec: build/h2_server
	./scripts/run_h2spec.sh

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

# Head to head vs cpp-httplib. Fetches their header on demand into
# benchmarks/third_party/ (gitignored) - the library stays dependency-free.
build/http_compare: benchmarks/http_compare.cpp $(HEADERS) | build
	./scripts/fetch_bench_deps.sh
	$(CXX) -std=$(STD) -Wall -Wextra -O3 -DNDEBUG -DCPPHTTPLIB_LISTEN_BACKLOG=1024 \
	    -Ibenchmarks/third_party -pthread benchmarks/http_compare.cpp -o $@ $(LDLIBS)

bench-http: build/http_compare
	./build/http_compare

# Multi-reactor scaling: N loops, N threads, one shared port (SO_REUSEPORT)
build/http_scale: benchmarks/http_scale.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/http_scale.cpp -o $@ $(LDLIBS)

bench-scale: build/http_scale
	./build/http_scale

# What one request costs in CPU alone - no sockets, no loop, no kernel
build/http_request_path: benchmarks/http_request_path.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/http_request_path.cpp -o $@ $(LDLIBS)

bench-request: build/http_request_path
	./build/http_request_path

# What typed signal dispatch costs against a raw epoll/kqueue loop
build/loop_dispatch: benchmarks/loop_dispatch.cpp $(HEADERS) | build
	$(CXX) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/loop_dispatch.cpp -o $@ $(LDLIBS)

bench-dispatch: build/loop_dispatch
	./build/loop_dispatch

# One self-contained file per entry header, for drop-in use
amalgamate:
	python3 scripts/amalgamate.py http/server.hpp -o single_include/ts_http_server.hpp
	python3 scripts/amalgamate.py ts_moveables.hpp

# The amalgamated header must always still compile - and run - on its own,
# so the single-file drop-in claim can never quietly drift
check-amalgamate: amalgamate | build
	printf '#include "ts_http_server.hpp"\nint main(){ snicholls::http::server s; s.get("/", [](const auto&, auto r){ r.send(200, "text/plain", "ok"); }); return s.listen("127.0.0.1", 0) ? 0 : 1; }\n' > build/amalgam_check.cpp
	$(CXX) -std=$(STD) -Wall -Wextra -O2 -pthread -Isingle_include build/amalgam_check.cpp -o build/amalgam_check
	./build/amalgam_check && echo "single-header drop-in: builds and runs"

clean:
	rm -rf build

.PHONY: all test check-msvc tsan asan demo bench demo-signals demo-capture demo-pcap demo-taskflow demo-timemaster demo-http demo-replay demo-drones test-tls check-mbedtls autobahn h2spec bench-http bench-scale bench-request bench-dispatch amalgamate check-amalgamate clean
