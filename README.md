# C++23 Lock-Free SPSC Queue

A high-performance, single-producer, single-consumer (SPSC) queue implemented in modern C++23.

This project provides a robust, tested, lock-free queue that is suitable for high-performance applications, such as real-time audio or low-latency trading systems, where data must be exchanged between two threads with minimal overhead.

## Features

-   **Lock-Free:** Uses `std::atomic` with correct memory ordering to ensure thread safety without mutexes, preventing deadlocks and priority inversion issues.
-   **Single-Producer, Single-Consumer (SPSC):** Optimized for the common two-thread communication pattern.
-   **Modern C++:** Uses modern features like `std::span` and `std::has_single_bit`.
-   **Header-Only:** The queue is provided as a single header file without any external dependences for easy integration.
-   **Cache-Friendly:** Atomic read/write pointers are aligned to cache lines to prevent "false sharing".
-   **`JUCE::AbstractFifo`-inspired Design:** The API manages two indices for a user-provided buffer, giving the user full control over memory allocation.
-   **Tested:** Includes a comprehensive test suite built with CMake and CTest.

## Core Concept: The Circular Buffer

The queue does not store data itself. It manages read and write indices for a memory buffer that you provide. This buffer is treated as a circle.

Imagine a buffer of size 8. `R` is the Read Index, `W` is the Write Index.

**1. Initial State:**
```
-R
-W
[-|-|-|-|-|-|-|-]
```

**2. After Writing 3 items (A, B, C):**
```
-R---W
[A|B|C|-|-|-|-|-]
```

**3. The "Wrap-Around" Problem**
Now, imagine the state is this, where `F` and `G` have been written, and items `a-e` have been read.
```
-----------R-W
[a|b|c|d|e|F|G|-]
```
If we want to write 3 new items (`X, Y, Z`), there isn't a single contiguous block of 3 free spaces. The queue must "wrap around" the end of the buffer. The `prepare_write(3)` method handles this by returning two blocks:

-   `blockSize1`: The chunk at the end of the buffer.
-   `blockSize2`: The chunk that wraps around to the beginning.

This is why the API returns two blocks — it efficiently handles this wrap-around case without needing to shuffle memory.

## How to Build and Run

This project uses CMake for building and CTest for running the test suite.

### Prerequisites

-   A C++23 compatible compiler (e.g., Clang 16+, GCC 13+).
-   CMake (version 3.22 or newer).
-   Git.

### Steps

1.  **Clone the repository:**
    ```sh
    git clone <your-repo-url>
    cd LockFreeSpscQueue
    ```

2.  **Configure CMake:**
    ```sh
    # This command configures the project. By default, it will enable 
    # building the tests and examples since the corresponding CMake options default to ON.
    cmake -S . -B build
    ```
    If you wish to disable building tests or examples (e.g., for a quicker configuration), you can turn the options off from the command line:
    ```sh
    cmake -S . -B build -DSPSC_QUEUE_BUILD_TESTS=OFF -DSPSC_QUEUE_BUILD_EXAMPLES=OFF
    ```

3.  **Build the project:**
    ```sh
    cmake --build build
    ```

4.  **Run the tests (if enabled):**
    ```sh
    cd build
    ctest --verbose
    ```

5.  **Run the examples (if enabled):**
    ```sh
    cd build/examples
    ./basic_usage
    ./advanced_usage
    ```

## How to Integrate Into Your Project

As a header-only library, integration is simple.

### Method 1: Git Submodule (Recommended)

This method keeps the library separate from your own source code and makes updates easy.

1.  **Add the repository as a submodule to your project:**
    ```sh
    # From the root of your project
    git submodule add <your-repo-url> external/LockFreeSpscQueue
    ```

2.  **In your project's main `CMakeLists.txt`, add the following:**
    ```cmake
    # Before adding the subdirectory, set the options to OFF. This prevents
    # your build from unnecessarily configuring and building the queue's own
    # tests and examples. This is the correct way to control options in a sub-project.
    set(SPSC_QUEUE_BUILD_TESTS OFF)
    set(SPSC_QUEUE_BUILD_EXAMPLES OFF)

    # Now add the subdirectory. It will respect the options set above.
    add_subdirectory(external/LockFreeSpscQueue)

    # ... define your own executable ...
    add_executable(MyAwesomeApp src/main.cpp)

    # Link your application to the spsc_queue library.
    # This automatically sets up the include directories.
    target_link_libraries(MyAwesomeApp PRIVATE spsc_queue)
    ```

### Method 2: Copy/Vendor

Simply copy the `include/` directory from this project into your project's source tree (e.g., under `external/` or `vendor/`) and add it to your include path.

**In your `CMakeLists.txt`:**
```cmake
# ... define your executable ...
add_executable(MyAwesomeApp src/main.cpp)

# Add the path to the copied headers
target_include_directories(MyAwesomeApp PRIVATE external/LockFreeSpscQueue/include)
```

## (Advanced) Performance Benchmarks

This project includes a performance benchmark suite using the [Google Benchmark](https://github.com/google/benchmark) library to measure queue throughput.

The benchmarks are **disabled by default** to keep configuration and build times fast for users who only want to integrate the library.

### How to Run Benchmarks

1.  **Configure CMake with benchmarks enabled:**
    You must explicitly enable the option `SPSC_QUEUE_BUILD_BENCHMARKS` when running CMake.

    ```sh
    # From the project root directory
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPSC_QUEUE_BUILD_BENCHMARKS=ON
    ```
    *Note: The first time you run this, CMake will download the Google Benchmark source code, which may take a moment.*

2.  **Build the project:**
    This will now build the `queue_benchmark` executable in addition to any other enabled targets.
    ```sh
    cmake --build build
    ```

3.  **Run the benchmark executable:**
    ```sh
    ./build/benchmarks/queue_benchmark
    ```

### Example Benchmark Output

You will see detailed output measuring the performance for different batch sizes. The most important column is `Items/s`, which shows the throughput in millions of items per second.

```
------------------------------------------------------------------------
Benchmark                  Time       CPU Iterations UserCounters...
------------------------------------------------------------------------
BM_QueueThroughput/1   0.042 ms  0.042 ms      16717 Items/s=195.043M/s
BM_QueueThroughput/4   0.011 ms  0.011 ms      66833 Items/s=777.743M/s
BM_QueueThroughput/16  0.003 ms  0.003 ms     266026 Items/s=3.10748G/s
BM_QueueThroughput/64  0.001 ms  0.001 ms    1049349 Items/s=12.3991G/s
BM_QueueThroughput/256 0.000 ms  0.000 ms    2211341 Items/s=25.924G/s

```
This output clearly shows how performance dramatically increases when transferring items in batches compared to one by one.

