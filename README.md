[![CI - Test Ubuntu/Linux](https://github.com/joz-k/LockFreeSpscQueue/actions/workflows/linux.yml/badge.svg)](https://github.com/joz-k/LockFreeSpscQueue/actions/workflows/linux.yml)
[![CI - Test macOS](https://github.com/joz-k/LockFreeSpscQueue/actions/workflows/macos.yml/badge.svg)](https://github.com/joz-k/LockFreeSpscQueue/actions/workflows/macos.yml)
[![CI - Test Windows](https://github.com/joz-k/LockFreeSpscQueue/actions/workflows/windows.yml/badge.svg)](https://github.com/joz-k/LockFreeSpscQueue/actions/workflows/windows.yml)

# C++23 Lock-Free SPSC Queue

A high-performance, _batch-oriented_, single-producer, single-consumer (SPSC) queue implemented in modern C++23.

This project provides a robust, tested, lock-free queue that is suitable for high-performance applications, such as real-time audio or low-latency trading systems, where data must be exchanged between two threads with minimal overhead.

## Features

-   **Lock-Free:** Uses `std::atomic` with correct memory ordering to ensure thread safety without mutexes, preventing deadlocks and priority inversion issues.
-   **Single-Producer, Single-Consumer (SPSC):** Optimized for the common two-thread communication pattern.
-   **Modern C++:** Uses modern features like `std::span` and `std::ranges`.
-   **Header-Only:** The queue is provided as a single header file without any external dependences for easy integration.
-   **Move Semantics Friendly:** The API design grants direct access to the buffer slots via `std::span` (in the `Scope` objects) and lambda arguments (in the `try_write`/`try_read` methods). This allows users to `std::move` objects into and out of the queue, providing a significant performance advantage over pointer-based APIs (which imply `memcpy`-style copies) when working with non-trivially-copyable types like `std::string`, `std::vector`, or `std::unique_ptr`.
-   **Cache-Friendly:** The queue is optimized for multi-core performance.
    1.  It uses `alignas` to place producer and consumer data on separate cache lines, preventing "false sharing."
    2.  It implements a performance optimization by **caching indices per core**. Each thread maintains a local, non-atomic cache of the other thread's position, minimizing expensive cross-core atomic operations.[^1]
-   **[`JUCE::AbstractFifo`][JuceFifo]-inspired Design:** The API manages two indices for a user-provided buffer, giving the user full control over memory allocation.
-   **Tested:** Includes a test suite built with CMake and CTest.
-   **Highly-Performant:** Optimized for batch-oriented use cases. It scales well with larger batches, often surpassing the [performance](benchmarks/README.md) of other industry-standard solutions.


[JuceFifo]: https://docs.juce.com/master/classAbstractFifo.html
[^1]: See ["MCRingBuffer"](https://www.cse.cuhk.edu.hk/~pclee/www/pubs/ancs09poster.pdf) paper or Rigtorp's [Optimizing a ring buffer for throughput](https://rigtorp.se/ringbuffer/).

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

**3. The "Wrap-Around" Problem:**
Now, imagine the state is this, where `F` and `G` have been written, and items `a-e` have been read.
```
-----------R-W
[a|b|c|d|e|F|G|-]
```
If we want to write 3 new items (`X, Y, Z`), there isn't a single contiguous block of 3 free spaces. The queue must "wrap around" the end of the buffer. The `prepare_write(3)` or `prepare_read(3)` methods handles this by returning two blocks:

-   `block1`: The chunk at the end of the buffer.
-   `block2`: The chunk that wraps around to the beginning.

This is why the API returns two blocks — it efficiently handles this wrap-around case without needing to shuffle memory.

Note: If there is no need to split the read or write operation into two chunks, only the `block1` is active and the `block2` span is returned as empty. For example, if you always write or read a single item, you can ignore `block2` altogether.

## Example Use

> [!TIP]
> Please refer to the "[Examples](examples/README.md)" section to explore more API options, including the
> new [Range-Based API](examples/README.md##4-range-based-api-stdranges), which completely abstracs away the two-block nature of the circular buffer.

Here is a complete, minimal example demonstrating the recommended batch-oriented usage. A producer thread sends several small batches of integers, and a consumer thread reads them as they become available. 

```cpp
#include "LockFreeSpscQueue.h"

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <numeric>

int main()
{
    // 1. Define the capacity for our queue. MUST be a power of two.
    const size_t QUEUE_CAPACITY = 128;

    // 2. Create the data buffer that will be shared between threads.
    std::vector<int> shared_data_buffer(QUEUE_CAPACITY);

    // 3. Create the queue manager, giving it a non-owning view of our buffer.
    LockFreeSpscQueue<int> queue(shared_data_buffer);

    // 4. Create a flag to signal when the producer is finished.
    std::atomic<bool> producer_is_done = false;

    // 5. Start the producer thread.
    //    std::jthread automatically joins on scope exit.
    std::jthread producer([&]() {
        std::cout << "Producer: Starting to send items in batches...\n";

        // Send 5 batches of 4 items each.
        for (int batch_num = 0; batch_num < 5; ++batch_num) {
            // Prepare a local batch of data.
            std::vector<int> local_batch(4);
            std::iota(local_batch.begin(), local_batch.end(), batch_num * 4); // Fills with 0,1,2,3 then 4,5,6,7 etc.

            std::cout << "Producer:   Attempting to send batch " << batch_num << " (items "
                      << local_batch.front() << "..." << local_batch.back() << ")\n";

            // Keep trying to write the entire batch until it succeeds.
            size_t items_written = 0;
            while (items_written < local_batch.size()) {
                // Create a view of the remaining items in our local batch.
                std::span<const int> sub_batch(local_batch.data() + items_written,
                                               local_batch.size() - items_written);

                // `try_write` will write as many items as it can and return the count.
                items_written += queue.try_write(sub_batch.size(), [&](auto block1, auto block2) {
                    std::copy_n(sub_batch.begin(), block1.size(), block1.begin());
                    if (!block2.empty()) {
                        std::copy_n(sub_batch.begin() + block1.size(), block2.size(), block2.begin());
                    }
                });
                
                // If the queue was full, `items_written` will not increase.
                // Yield to give the consumer a chance to run.
                if (items_written < local_batch.size()) {
                    std::this_thread::yield();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "Producer: Finished.\n";
        producer_is_done.store(true, std::memory_order_release);
    });

    // 6. Start the consumer thread.
    std::jthread consumer([&]() {
        std::cout << "Consumer: Waiting for items...\n";
        while (true) {
            // Try to read a batch of up to 16 items at a time.
            const size_t items_read = queue.try_read(16, [&](auto block1, auto block2) {
                // Process all items in the first contiguous block.
                for (int item : block1) {
                    std::cout << "Consumer: Got  " << item << "\n";
                }
                // Process all items in the second (wrapped-around) block.
                for (int item : block2) {
                    std::cout << "Consumer: Got  " << item << "\n";
                }
            });

            if (items_read == 0) {
                // Queue was empty. If the producer is also done, we can exit.
                if (producer_is_done.load(std::memory_order_acquire)) {
                    // One final check to prevent a race condition.
                    if (queue.get_num_items_ready() == 0) {
                        break;
                    }
                } else {
                    // Producer is still working, but the queue is empty.
                    // Wait for a moment to prevent a high-CPU spin-loop.
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        std::cout << "Consumer: Finished.\n";
    });
}
```

## How to Build and Run

This project uses CMake for building and CTest for running the test suite.

### Prerequisites

-   A C++23 compatible compiler (e.g., Clang 16+, GCC 13+, MSVC 19.44+).
-   CMake (version 3.22 or newer).
-   Git.

### Steps

1.  **Clone the repository:**
    ```sh
    git clone https://github.com/joz-k/LockFreeSpscQueue.git
    cd LockFreeSpscQueue
    ```

2.  **Configure CMake:**
    ```sh
    # This command configures the project. By default, it will enable 
    # building the tests and examples since the corresponding CMake options default to ON.
    cmake -S . -B build
    # Or if you want to use Ninja instead of Make:
    cmake -G Ninja -S . -B build
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
    git submodule add https://github.com/joz-k/LockFreeSpscQueue.git external/LockFreeSpscQueue
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

## Performance Benchmarks

This project includes a benchmark suite, using the [Google Benchmark](https://github.com/google/benchmark) library, to measure queue throughput and compare it against [`moodycamel::ReaderWriterQueue`](https://github.com/cameron314/readerwriterqueue). For instructions on how to run the benchmarks, see the [Benchmarks](benchmarks/README.md) section.

## Disclaimers

This code was only tested on x86\_64 and ARM64 CPU architectures. I also did not try running it on operating systems other than Linux, macOS, and Windows.

## Similar Projects

* <https://github.com/juce-framework/JUCE/blob/master/modules/juce_core/containers/juce_AbstractFifo.h>
* <https://github.com/steinwurf/boost/tree/master/boost/lockfree>
* <https://github.com/cameron314/readerwriterqueue>
* <https://github.com/MayaPosch/LockFreeRingBuffer>
* <https://github.com/facebook/folly/blob/main/folly/ProducerConsumerQueue.h>
* <https://github.com/rigtorp/SPSCQueue>
* <https://github.com/Deaod/spsc_queue>

