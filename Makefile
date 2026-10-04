CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  ?= -pthread
SAN      := -std=c++20 -O1 -g -Wall -Wextra -Iinclude -fno-omit-frame-pointer
HDRS     := $(wildcard include/tickforge/*.hpp) tests/reference_book.hpp tests/check.hpp bench/flow.hpp

TESTS   := build/test_book build/test_differential build/test_feed build/test_spsc
BENCHES := build/bench_book build/bench_pipeline build/itch_replay

all: $(TESTS) $(BENCHES) build/demo

build/%: tests/%.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

build/%: bench/%.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Same replay, instrumented to record how far from the best price each lookup lands.
build/itch_depth: bench/itch_replay.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DTICKFORGE_DEPTH_STATS $< -o $@

build/demo: examples/demo.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

bench: build/bench_book build/bench_pipeline
	./build/bench_book
	./build/bench_pipeline

# AddressSanitizer + UndefinedBehaviorSanitizer over the single-threaded tests.
asan:
	@mkdir -p build
	$(CXX) $(SAN) -fsanitize=address,undefined tests/test_book.cpp -o build/asan_book
	$(CXX) $(SAN) -fsanitize=address,undefined tests/test_differential.cpp -o build/asan_diff
	$(CXX) $(SAN) -fsanitize=address,undefined tests/test_feed.cpp -o build/asan_feed
	./build/asan_book && ./build/asan_diff 10 100000 && ./build/asan_feed

# ThreadSanitizer over the lock-free queue.
tsan:
	@mkdir -p build
	$(CXX) $(SAN) -fsanitize=thread tests/test_spsc.cpp -o build/tsan_spsc $(LDFLAGS)
	./build/tsan_spsc 2000000

clean:
	rm -rf build

.PHONY: all test bench asan tsan clean
