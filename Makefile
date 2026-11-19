# TSMoveables - Thread Safe Moveables
#
# The library itself is header-only (TSMoveables/*.hpp).
# This Makefile builds the demo and the cassert-based unit tests.
#
#   make test    build and run the unit tests
#   make tsan    build and run the unit tests under ThreadSanitizer
#   make asan    build and run the unit tests under Address+UB Sanitizers
#   make demo    build and run the demo (same code as the Xcode target)
#   make demo-remote  remote execution: client and workers over loopback (C++20, POSIX)
#   make test-collective  allreduce, broadcast, barrier and all-to-all over TCP (C++20, POSIX)
#   make clean
#
#   make test STD=c++17    any target can be built against a different standard

CXX      ?= c++
STD      ?= c++20
CXXFLAGS ?= -std=$(STD) -Wall -Wextra -pedantic -O2 -g

# Appended rather than folded into CXXFLAGS, because CXXFLAGS is ?= and a
# caller overriding it would otherwise drop -std and the warning flags with it.
# CI passes EXTRA_FLAGS=-Werror: a warning that nobody has to act on is a
# warning that stays forever, and this library is deployed somewhere that
# cannot afford one to be the interesting kind.
EXTRA_FLAGS ?=
CXXFLAGS += $(EXTRA_FLAGS)

# snicholls-core (third_party/core, a submodule): constexpr_for lives there. Header-only, standard library only.
# Every compile needs it, including the recipes below that spell out their own flags instead of using CXXFLAGS.
CORE_INC := -Ithird_party/core/include
CXXFLAGS += $(CORE_INC)
ifneq ($(MAKECMDGOALS),clean)
  ifeq ($(wildcard third_party/core/include/snicholls/core/constexpr_for.hpp),)
    $(error third_party/core is empty: run `git submodule update --init` (or clone with --recurse-submodules))
  endif
endif

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

build/tests_o0: $(TEST_SRC) $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O0 -g $(EXTRA_FLAGS) -pthread $(TEST_SRC) -o $@ $(LDLIBS)

build/demo: TSMoveables/main.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -pthread TSMoveables/main.cpp -o $@ $(LDLIBS)

build/bench: benchmarks/bench.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread benchmarks/bench.cpp -o $@ $(LDLIBS)

# Head-to-head vs moodycamel. Fetches the third-party headers first (gitignored).
# Warning flags relaxed here because the third-party headers are not ours to lint.
benchmarks/third_party/concurrentqueue.h:
	bash scripts/fetch_bench_deps.sh

build/bench_compare: benchmarks/bench.cpp $(HEADERS) benchmarks/third_party/concurrentqueue.h | build
	$(CXX) $(CORE_INC) -std=$(STD) -O3 -DNDEBUG -DTS_BENCH_COMPARE -Ibenchmarks/third_party -pthread benchmarks/bench.cpp -o $@ $(LDLIBS)

build/signal_slot_demo: demos/signal_slot_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/signal_slot_demo.cpp -o $@ $(LDLIBS)

build/capture_replay_demo: demos/capture_replay_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/capture_replay_demo.cpp -o $@ $(LDLIBS)

build/pcap_replay_demo: demos/pcap_replay_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/pcap_replay_demo.cpp -o $@ $(LDLIBS)

build/taskflow_style_demo: demos/taskflow_style_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/taskflow_style_demo.cpp -o $@ $(LDLIBS)

build/time_master_demo: demos/time_master_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/time_master_demo.cpp -o $@ $(LDLIBS)

build/drone_fleet_demo: demos/drone_fleet_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    demos/drone_fleet_demo.cpp -o $@ $(LDLIBS)

demo-drones: build/drone_fleet_demo
	./build/drone_fleet_demo

build/replay_loop_demo: demos/replay_loop_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    demos/replay_loop_demo.cpp -o $@ $(LDLIBS)

demo-replay: build/replay_loop_demo
	./build/replay_loop_demo

build/http_server_demo: demos/http_server_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/http_server_demo.cpp -o $@ $(LDLIBS)

build/webhook_ws_hub_demo: demos/webhook_ws_hub_demo.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread demos/webhook_ws_hub_demo.cpp -o $@ $(LDLIBS)

# A static check for the one MSVC rule this repository keeps tripping over
check-msvc:
	python3 scripts/check_msvc_capture.py

# Every tests/tests_*.cpp must be listed in CMakeLists.txt, or the CMake jobs
# fail at link time while `make test` stays green. Checked, not remembered.
check-tests:
	python3 scripts/check_test_registry.py

# The version is written down in version.hpp, CMakeLists.txt and the git tag.
# One release, one number - checked rather than remembered.
check-version:
	python3 scripts/check_version.py

# The same suite with the optimiser off. Not redundant with `test`: -O0 keeps
# different code, schedules threads differently and widens the windows a race
# has to be caught in, and an assert that -O2 folded away is live again here.
test-debug: build/tests_o0
	./build/tests_o0

# Race detection is probabilistic, so one green TSan run is weak evidence for a
# library whose whole subject is concurrency. This runs only the suites whose
# bugs are interleavings - 93 of the 214 - repeatedly under TSan, which is the
# cheapest way to turn "no race observed once" into something worth saying.
STRESS_RUNS ?= 20
stress: build/tests_tsan
	@for i in $$(seq 1 $(STRESS_RUNS)); do \
	    printf 'tsan concurrency run %s/%s ... ' "$$i" "$(STRESS_RUNS)"; \
	    TSAN_OPTIONS="suppressions=tests/tsan.supp halt_on_error=1" \
	        ./build/tests_tsan concurrency > /tmp/stress.$$$$.log 2>&1 \
	        || { echo FAILED; cat /tmp/stress.$$$$.log; rm -f /tmp/stress.$$$$.log; exit 1; }; \
	    echo ok; rm -f /tmp/stress.$$$$.log; \
	done
	@echo "$(STRESS_RUNS) TSan runs of the concurrency suites: clean"

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

# ws vs wss on the same path. Lives with the TLS binaries because it links
# OpenSSL; the core benchmarks stay dependency-free.
build/ws_tls_throughput: benchmarks/tls/ws_tls_throughput.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    -I$(OPENSSL_PREFIX)/include benchmarks/tls/ws_tls_throughput.cpp -o $@ \
	    -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto $(LDLIBS)

bench-wss: build/ws_tls_throughput
	./build/ws_tls_throughput

test-tls: build/tests_tls
	@echo "backends compiled: openssl$(if $(filter 1,$(MBEDTLS_FOUND)), + mbedtls, ONLY - mbedTLS absent)"
	./build/tests_tls

# Autobahn|Testsuite - the external RFC 6455 grader. The echo server is ours;
# the fuzzing client comes from the suite (venv or Docker, see the script).
# The echo server is built with permessage-deflate wired in (zlib), so the
# Autobahn compression groups are graded rather than skipped
build/ws_echo_server: tests/autobahn/ws_echo_server.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O2 -pthread -DSNICHOLLS_AUTOBAHN_DEFLATE \
	    tests/autobahn/ws_echo_server.cpp -o $@ -lz $(LDLIBS)

autobahn: build/ws_echo_server
	./scripts/run_autobahn.sh

# h2spec - the external RFC 9113 / RFC 7541 grader. Same shape as Autobahn:
# the server under test is ours, the conformance client comes from the
# official image, and the runner distinguishes "could not run" from "failed"
build/h2_server: tests/h2spec/h2_server.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O2 -pthread \
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

build/bwt_dna_demo: demos/bwt_dna_demo.cpp demos/text_index/suffix_array.hpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    demos/bwt_dna_demo.cpp -o $@ $(LDLIBS)

demo-bwt: build/bwt_dna_demo
	./build/bwt_dna_demo

demo-webhook: build/webhook_ws_hub_demo
	./build/webhook_ws_hub_demo

# Remote execution (demos/remote_compute): remote std::async / std::invoke over
# this library's TCP and thread pools. C++20 whatever STD says (it uses class-
# type template parameters; the library itself stays C++17) and POSIX sockets.
# `make demo-remote` is its self-test: the client and every worker in one
# process, talking over 127.0.0.1 - which is how CI runs it on one machine.
REMOTE_DIR  := demos/remote_compute
REMOTE_STD  ?= c++20
REMOTE_HDRS := $(wildcard $(REMOTE_DIR)/*.hpp $(REMOTE_DIR)/remote/*.hpp)
REMOTE_INC  := $(CORE_INC) -I$(REMOTE_DIR) -ITSMoveables

build/remote_compute_demo: $(REMOTE_DIR)/remote_compute_demo.cpp $(REMOTE_HDRS) $(HEADERS) | build
	$(CXX) $(REMOTE_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -g $(EXTRA_FLAGS) -pthread \
	    $(REMOTE_DIR)/remote_compute_demo.cpp -o $@ $(LDLIBS)

build/remote_compute_demo_tsan: $(REMOTE_DIR)/remote_compute_demo.cpp $(REMOTE_HDRS) $(HEADERS) | build
	$(CXX) $(REMOTE_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O1 -g -fsanitize=thread -pthread \
	    $(REMOTE_DIR)/remote_compute_demo.cpp -o $@ $(LDLIBS)

build/remote_compute_bench: $(REMOTE_DIR)/remote_compute_bench.cpp $(REMOTE_HDRS) $(HEADERS) | build
	$(CXX) $(REMOTE_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -DNDEBUG $(EXTRA_FLAGS) -pthread \
	    $(REMOTE_DIR)/remote_compute_bench.cpp -o $@ $(LDLIBS)

# --no-lan: skip the multicast discovery check, which needs a network that
# carries multicast (CI runners and containers often do not)
demo-remote: build/remote_compute_demo
	./build/remote_compute_demo selftest --no-lan

tsan-remote: build/remote_compute_demo_tsan
	TSAN_OPTIONS="suppressions=tests/tsan.supp halt_on_error=1" ./build/remote_compute_demo_tsan selftest --no-lan

# Where the time in a remote call goes, layer by layer, on loopback
bench-remote: build/remote_compute_bench
	./build/remote_compute_bench

# Memory-mapped storage (TSMoveables/mmap/): C++20 and POSIX, like the remote
# demo, so its tests are a program of their own rather than part of build/tests
# (which also builds as C++17). Needs snicholls-core with containers/
# index_pool.hpp and mapped_header.hpp. `make test-mmap` runs them; set
# TSM_MMAP_SMALL_FS to a small filesystem to include the full-disk test.
MMAP_HDRS := $(wildcard TSMoveables/mmap/*.hpp) TSMoveables/fs/sync.hpp
MMAP_INC  := $(CORE_INC) -ITSMoveables

build/tests_mmap: tests/mmap/tests_mmap.cpp $(MMAP_HDRS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -g $(EXTRA_FLAGS) \
	    tests/mmap/tests_mmap.cpp -o $@ $(LDLIBS)

build/tests_mmap_asan: tests/mmap/tests_mmap.cpp $(MMAP_HDRS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O1 -g -fsanitize=address,undefined \
	    -fno-sanitize-recover=all tests/mmap/tests_mmap.cpp -o $@ $(LDLIBS)

build/mmap_bench: benchmarks/mmap_bench.cpp $(MMAP_HDRS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O3 -DNDEBUG $(EXTRA_FLAGS) \
	    benchmarks/mmap_bench.cpp -o $@ $(LDLIBS)

# Whole-file reads and atomic replacement (TSMoveables/fs/): C++20 and POSIX,
# like mmap/, so a program of its own. `make test-fs` runs it.
FS_HDRS := $(wildcard TSMoveables/fs/*.hpp)

build/tests_fs: tests/fs/tests_fs.cpp $(FS_HDRS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -g $(EXTRA_FLAGS) -pthread \
	    tests/fs/tests_fs.cpp -o $@ $(LDLIBS)

build/tests_fs_asan: tests/fs/tests_fs.cpp $(FS_HDRS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O1 -g -pthread -fsanitize=address,undefined \
	    -fno-sanitize-recover=all tests/fs/tests_fs.cpp -o $@ $(LDLIBS)

test-fs: build/tests_fs
	./build/tests_fs

asan-fs: build/tests_fs_asan
	./build/tests_fs_asan

# Process lifecycle (TSMoveables/process/: signals, exit codes, .env, config,
# environment, health and the boot gate): C++20 and POSIX, and the tests raise
# real signals and fork death tests, so a program of their own.
PROC_HDRS := $(wildcard TSMoveables/process/*.hpp) $(FS_HDRS) $(wildcard tests/process/*.hpp)
PROC_SRC  := $(wildcard tests/process/*.cpp)

build/tests_process: $(PROC_SRC) $(PROC_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -g $(EXTRA_FLAGS) -pthread \
	    $(PROC_SRC) -o $@ $(LDLIBS)

build/tests_process_asan: $(PROC_SRC) $(PROC_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O1 -g -pthread -fsanitize=address,undefined \
	    -fno-sanitize-recover=all $(PROC_SRC) -o $@ $(LDLIBS)

test-process: build/tests_process
	./build/tests_process

asan-process: build/tests_process_asan
	./build/tests_process_asan

# HTTP client (TSMoveables/http/client.hpp, HTTP/1.1 and HTTP/2): C++20, like
# process/. Its tests run against http::server, our h2 server, and scripted
# raw-socket servers.
CLIENT_HDRS := TSMoveables/http/client.hpp TSMoveables/http/h2_client.hpp TSMoveables/http/h2_wire.hpp \
    TSMoveables/net/outbound.hpp TSMoveables/net/url.hpp
CLIENT_SRC := tests/client/tests_http_client.cpp tests/client/tests_h2_client.cpp
CLIENT_TESTS := $(CLIENT_SRC) tests/client/client_harness.hpp

build/tests_http_client: $(CLIENT_TESTS) $(CLIENT_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -g $(EXTRA_FLAGS) -pthread \
	    $(CLIENT_SRC) -o $@ $(LDLIBS)

build/tests_http_client_asan: $(CLIENT_TESTS) $(CLIENT_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O1 -g -pthread -fsanitize=address,undefined \
	    -fno-sanitize-recover=all $(CLIENT_SRC) -o $@ $(LDLIBS)

test-client: build/tests_http_client
	./build/tests_http_client

# The client's HTTP/2 against servers that are not ours: Go's standard library
# (h2c, and h2 over TLS by ALPN) and nghttpd when present. Graders, not
# dependencies - the same footing as h2spec and Autobahn.
build/h2_client_interop: tests/client/h2_interop/interop.cpp $(CLIENT_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -g $(EXTRA_FLAGS) -pthread \
	    -I$(OPENSSL_PREFIX)/include tests/client/h2_interop/interop.cpp -o $@ \
	    -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto $(LDLIBS)

h2-client-interop: build/h2_client_interop
	./scripts/h2_client_interop.sh

asan-client: build/tests_http_client_asan
	./build/tests_http_client_asan

# Collectives over TCP (TSMoveables/net/collective.hpp): a full mesh, allreduce_sum in rank
# order, broadcast, barrier and all_to_all_v. C++20 and POSIX, like http::client, so a program of its own.
# The tests run 1 to 5 ranks as threads on 127.0.0.1 (one is a forked process, killed);
# the demo runs ranks as processes; the benchmark moves 0.5 GB on 2 and 4 processes.
COLL_HDRS := TSMoveables/net/collective.hpp TSMoveables/net/tcp.hpp

build/tests_collective: tests/net/tests_collective.cpp $(COLL_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -g $(EXTRA_FLAGS) -pthread \
	    tests/net/tests_collective.cpp -o $@ $(LDLIBS)

build/tests_collective_asan: tests/net/tests_collective.cpp $(COLL_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O1 -g -pthread -fsanitize=address,undefined \
	    -fno-sanitize-recover=all tests/net/tests_collective.cpp -o $@ $(LDLIBS)

build/tests_collective_tsan: tests/net/tests_collective.cpp $(COLL_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O1 -g -pthread -fsanitize=thread \
	    tests/net/tests_collective.cpp -o $@ $(LDLIBS)

build/collective_demo: demos/collective_demo.cpp $(COLL_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 -g $(EXTRA_FLAGS) -pthread \
	    demos/collective_demo.cpp -o $@ $(LDLIBS)

build/collective_bench: benchmarks/collective_bench.cpp $(COLL_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O3 -DNDEBUG $(EXTRA_FLAGS) -pthread \
	    benchmarks/collective_bench.cpp -o $@ $(LDLIBS)

test-collective: build/tests_collective
	./build/tests_collective

asan-collective: build/tests_collective_asan
	./build/tests_collective_asan

# Linux evidence: on a macOS 26 host Apple's TSan runtime traps before main
tsan-collective: build/tests_collective_tsan
	TSAN_OPTIONS="suppressions=tests/tsan.supp halt_on_error=1" ./build/tests_collective_tsan

demo-collective: build/collective_demo
	./build/collective_demo selftest

bench-collective: build/collective_bench
	./build/collective_bench

# The persistent full-text index (demos/text_index): a suffix array in
# mapped files, built once and reopened in microseconds. Self-verifying.
build/persistent_index_demo: demos/text_index/persistent_index_demo.cpp demos/text_index/suffix_array.hpp $(MMAP_HDRS) $(HEADERS) | build
	$(CXX) $(MMAP_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O3 -DNDEBUG $(EXTRA_FLAGS) -pthread \
	    demos/text_index/persistent_index_demo.cpp -o $@ $(LDLIBS)

demo-text-index: build/persistent_index_demo
	./build/persistent_index_demo

test-mmap: build/tests_mmap
	./build/tests_mmap

asan-mmap: build/tests_mmap_asan
	./build/tests_mmap_asan

bench-mmap: build/mmap_bench
	./build/mmap_bench

# Byte-level BPE (demos/tokenizer): C++20, like the remote demo. Needs a data
# directory from scripts/tokenizer/prepare.py (Python, with tiktoken and
# rustbpe), so it is not in the self-verifying set CI runs:
#     python scripts/tokenizer/prepare.py build/tokdata
#     make bench-tokenizer TOKDATA=build/tokdata
TOK_DIR  := demos/tokenizer
TOK_HDRS := $(wildcard $(TOK_DIR)/tok/*.hpp)
TOK_INC  := $(CORE_INC) -I$(TOK_DIR) -ITSMoveables
TOKDATA  ?= build/tokdata

build/tokenizer_bench: $(TOK_DIR)/tokenizer_bench.cpp $(TOK_HDRS) $(HEADERS) | build
	$(CXX) $(TOK_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O3 -DNDEBUG $(EXTRA_FLAGS) -pthread \
	    $(TOK_DIR)/tokenizer_bench.cpp -o $@ $(LDLIBS)

build/long_merge_check: $(TOK_DIR)/long_merge_check.cpp $(TOK_HDRS) $(HEADERS) | build
	$(CXX) $(TOK_INC) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O3 -DNDEBUG $(EXTRA_FLAGS) -pthread \
	    $(TOK_DIR)/long_merge_check.cpp -o $@ $(LDLIBS)

build/split_check: $(TOK_DIR)/split_check.cpp $(TOK_HDRS) | build
	$(CXX) -I$(TOK_DIR) -std=$(REMOTE_STD) -Wall -Wextra -pedantic -O2 $(EXTRA_FLAGS) \
	    $(TOK_DIR)/split_check.cpp -o $@

# The splitter's fast path against its reference, then every document checked
# against tiktoken's tokens and timed, for both vocabularies
bench-tokenizer: build/split_check build/tokenizer_bench build/long_merge_check
	./build/split_check $(TOKDATA)/docs.bin
	./build/tokenizer_bench $(TOKDATA) cl100k
	./build/tokenizer_bench $(TOKDATA) rust32k
	./build/long_merge_check $(TOKDATA) cl100k

build/ws_hub_broadcast: benchmarks/ws_hub_broadcast.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/ws_hub_broadcast.cpp -o $@ $(LDLIBS)

build/parallel_scaling: benchmarks/parallel_scaling.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/parallel_scaling.cpp -o $@ $(LDLIBS)

bench-parallel: build/parallel_scaling
	./build/parallel_scaling

build/pool_dispatch: benchmarks/pool_dispatch.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/pool_dispatch.cpp -o $@ $(LDLIBS)

bench-pool-dispatch: build/pool_dispatch
	./build/pool_dispatch

bench-wshub: build/ws_hub_broadcast
	./build/ws_hub_broadcast

build/ws_relay_scaling: benchmarks/ws_relay_scaling.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/ws_relay_scaling.cpp -o $@ $(LDLIBS)

bench-relay: build/ws_relay_scaling
	./build/ws_relay_scaling

# Memory has to be measured in a process that has done nothing else - RSS
# deltas between sweeps in one process read near zero, because the allocator
# reuses pages it already faulted in.
bench-relay-memory: build/ws_relay_scaling
	./build/ws_relay_scaling --memory 2000

build/rpc_latency: benchmarks/rpc_latency.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/rpc_latency.cpp -o $@ $(LDLIBS)

bench-rpc: build/rpc_latency
	./build/rpc_latency

# Head to head vs cpp-httplib. Fetches their header on demand into
# benchmarks/third_party/ (gitignored) - the library stays dependency-free.
build/http_compare: benchmarks/http_compare.cpp $(HEADERS) | build
	./scripts/fetch_bench_deps.sh
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -O3 -DNDEBUG -DCPPHTTPLIB_LISTEN_BACKLOG=1024 \
	    -Ibenchmarks/third_party -pthread benchmarks/http_compare.cpp -o $@ $(LDLIBS)

bench-http: build/http_compare
	./build/http_compare

# Multi-reactor scaling: N loops, N threads, one shared port (SO_REUSEPORT)
build/http_scale: benchmarks/http_scale.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/http_scale.cpp -o $@ $(LDLIBS)

bench-scale: build/http_scale
	./build/http_scale


# What one request costs in CPU alone - no sockets, no loop, no kernel
build/http_request_path: benchmarks/http_request_path.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/http_request_path.cpp -o $@ $(LDLIBS)

bench-request: build/http_request_path
	./build/http_request_path

# What typed signal dispatch costs against a raw epoll/kqueue loop
build/loop_dispatch: benchmarks/loop_dispatch.cpp $(HEADERS) | build
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -pedantic -O3 -DNDEBUG -pthread \
	    benchmarks/loop_dispatch.cpp -o $@ $(LDLIBS)

bench-dispatch: build/loop_dispatch
	./build/loop_dispatch

# One self-contained file per entry header, for drop-in use
amalgamate:
	python3 scripts/amalgamate.py http/server.hpp -o single_include/ts_http_server.hpp
	python3 scripts/amalgamate.py ts_moveables.hpp

# The amalgamated headers must always still compile - and run - on their own,
# and must contain the library rather than just the part that already worked:
# check_amalgamation.py proves every header in the tree is in the file, and
# tests/amalgamation/drop_in.cpp uses every shipped component through it
check-amalgamate: amalgamate | build
	python3 scripts/check_amalgamation.py
	printf '#include "ts_http_server.hpp"\nint main(){ snicholls::http::server s; s.get("/", [](const auto&, auto r){ r.send(200, "text/plain", "ok"); }); return s.listen("127.0.0.1", 0) ? 0 : 1; }\n' > build/amalgam_http_check.cpp
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -O2 -pthread -Isingle_include build/amalgam_http_check.cpp -o build/amalgam_http_check
	./build/amalgam_http_check && echo "single-header http drop-in: builds and runs"
	$(CXX) $(CORE_INC) -std=$(STD) -Wall -Wextra -O2 -pthread -Isingle_include tests/amalgamation/drop_in.cpp -o build/amalgam_check
	./build/amalgam_check

clean:
	rm -rf build

.PHONY: all test test-debug stress check-msvc check-tests tsan asan demo bench demo-signals demo-capture demo-pcap demo-taskflow demo-timemaster demo-http demo-webhook demo-remote tsan-remote bench-remote bench-tokenizer demo-bwt demo-replay demo-drones test-tls check-mbedtls check-version autobahn h2spec bench-http bench-scale bench-request bench-dispatch bench-wshub bench-parallel bench-pool-dispatch bench-relay bench-relay-memory bench-wss bench-rpc test-mmap asan-mmap bench-mmap test-fs asan-fs test-process asan-process test-client asan-client test-collective asan-collective tsan-collective demo-collective bench-collective h2-client-interop demo-text-index amalgamate check-amalgamate clean
