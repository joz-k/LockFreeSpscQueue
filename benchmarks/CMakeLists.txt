# This file handles the building of performance benchmarks.

# 1. Fetch Google Benchmark (needed for all benchmarks)
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

# 3. Build the standard, internal benchmark if enabled
if(SPSC_QUEUE_BUILD_BENCHMARKS)
    add_executable(queue_benchmark queue_benchmark.cpp)
    target_link_libraries(queue_benchmark PRIVATE
        spsc_queue
        benchmark::benchmark
        Threads::Threads
    )
endif()

# 4. Build the new comparative benchmark if enabled
if(SPSC_QUEUE_BUILD_BENCHMARK_COMPARE)
    # Fetch moodycamel::ConcurrentQueue
    FetchContent_Declare(
        MoodycamelConcurrentQueue
        GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
        GIT_TAG        v1.0.7 # Use a specific tag for stability
    )
    FetchContent_MakeAvailable(MoodycamelConcurrentQueue)

    # Define the comparative benchmark executable
    add_executable(queue_comparison_benchmark queue_comparison_benchmark.cpp)

    # Link it against our queue, the moodycamel queue, Google Benchmark, and threads
    target_link_libraries(queue_comparison_benchmark PRIVATE
        spsc_queue
        readerwriterqueue
        benchmark::benchmark
        Threads::Threads
    )
endif()

