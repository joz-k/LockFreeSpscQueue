# This file handles the building of performance benchmarks.

# 1. Fetch Google Benchmark
# Use FetchContent to download and prepare Google Benchmark. This is the modern
# CMake approach and does not require the user to have it installed system-wide.
include(FetchContent)
FetchContent_Declare(
    GoogleBenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3 # Using a specific tag ensures stability
)

# Disable building of Google Benchmark's own internal tests. We only need the library.
# This variable must be set *before* FetchContent_MakeAvailable is called.
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable testing for Google Benchmark")

# This makes the targets from Google Benchmark available for us to use.
FetchContent_MakeAvailable(GoogleBenchmark)

# 2. Find the threading library
find_package(Threads REQUIRED)

# 3. Define the benchmark executable
add_executable(queue_benchmark queue_benchmark.cpp)

# 4. Link the executable against our queue, Google Benchmark, and threads.
#    The `benchmark::benchmark` target is created by FetchContent_MakeAvailable.
target_link_libraries(queue_benchmark PRIVATE
    spsc_queue
    benchmark::benchmark
    Threads::Threads
)
