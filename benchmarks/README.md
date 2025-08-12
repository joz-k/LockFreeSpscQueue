# Benchmarks

This directory contains the performance benchmark suite for the `LockFreeSpscQueue` project, built using the [Google Benchmark](https://github.com/google/benchmark) library.

It includes:

The suite includes two primary executables:
1.  **`queue_benchmark`:** A simple performance benchmark that measures the raw throughput of this library's batching API.
2.  **`queue_comparison_benchmark`:** A comparative benchmark that stress-tests this library against the industry-standard [moodycamel::readerwriterqueue](https://github.com/cameron314/readerwriterqueue).

The benchmarks are **disabled by default** to keep configuration and build times fast for users who only want to integrate the library.

### How to Run

It is **critical** to use the `Release` build type, as benchmarking a `Debug` build will produce meaningless results.

1.  **Configure CMake:**
    You must explicitly enable the desired benchmark option when running CMake.

    ```sh
    # To build ONLY the internal benchmark:
    cmake -S .. -B build -DCMAKE_BUILD_TYPE=Release -DSPSC_QUEUE_BUILD_BENCHMARKS=ON

    # To build ONLY the comparison benchmark (most common):
    cmake -S .. -B build -DCMAKE_BUILD_TYPE=Release -DSPSC_QUEUE_BUILD_BENCHMARK_COMPARE=ON
    ```
    *Note: The first time you run this, CMake will download the required dependency libraries, which may take a moment.*

2.  **Build the Project:**
    ```sh
    cmake --build build --config Release
    ```

3.  **Run the Executables:**
    ```sh
    # On Linux/macOS
    ./build/benchmarks/queue_comparison_benchmark

    # On Windows
    .\build\benchmarks\Release\queue_comparison_benchmark.exe
    ```

---

## Raw Queue Speed Result

When running the `queue_benchmarks`, you will see detailed output measuring the performance for different batch sizes. The most important column is `Items/s`, which shows the throughput in millions of items per second.

Example output:

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

## Performance Comparison: Results & Analysis

The following are example results from running the `queue_comparison_benchmark` on an Apple MacBook Air (M2). The benchmark transfers a sequence of pseudo-random `int64_t` values and simultaneously verifies data integrity, making it both a performance and correctness stress test.

### Benchmark Data

```
------------------------------------------------------------------------------------------
Benchmark                            Time      CPU Iterations UserCounters...
------------------------------------------------------------------------------------------
BM_ThisQueue_SingleItem           2.32 ms  2.32 ms        320 items_per_second=43.2044M/s
BM_ThisQueue_SingleItem_Write256 0.108 ms 0.108 ms       6466 items_per_second=924.364M/s
BM_Moodycamel_SingleItem         0.245 ms 0.245 ms       2932 items_per_second=408.063M/s
BM_ThisQueue_Batch/4             0.686 ms 0.686 ms        983 items_per_second=145.91M/s
BM_ThisQueue_Batch/8             0.151 ms 0.151 ms       4561 items_per_second=662.117M/s
BM_ThisQueue_Batch/16            0.114 ms 0.114 ms       6597 items_per_second=878.876M/s
BM_ThisQueue_Batch/64            0.093 ms 0.093 ms       7487 items_per_second=1.07577G/s
BM_ThisQueue_Batch/256           0.086 ms 0.086 ms       8186 items_per_second=1.16642G/s
BM_Moodycamel_Batch/4            0.243 ms 0.243 ms       2863 items_per_second=412.17M/s
BM_Moodycamel_Batch/8            0.239 ms 0.239 ms       2933 items_per_second=418.038M/s
BM_Moodycamel_Batch/16           0.240 ms 0.240 ms       2896 items_per_second=416.377M/s
BM_Moodycamel_Batch/64           0.242 ms 0.242 ms       2880 items_per_second=413.098M/s
BM_Moodycamel_Batch/256          0.242 ms 0.242 ms       2895 items_per_second=414.29M/s
```

### Key Takeaways

The benchmark data reveals a clear performance profile based on the architectural trade-offs of each library.

*   **1. Single-Item Transfers (`ThisQueue_SingleItem`):** For individual, non-batched item transfers, `moodycamel::ReaderWriterQueue` shows significantly higher throughput (`~412 M/s`) compared to this library's low-level `prepare_write` path (`~44 M/s`). This performance difference is attributed to a fundamental architectural distinction. `moodycamel::ReaderWriterQueue` uses a "queue-of-queues" design (a linked list of smaller ring buffers). This structure provides a higher degree of decoupling, allowing its producer to often operate on a local block with minimal need to synchronize with the consumer's global position. In contrast, this library uses a single, contiguous ring buffer, which results in a tighter coupling between the producer and consumer states, leading to a higher per-transaction cost for single items.

*   **2. Transactional Writes (`ThisQueue_SingleItem_Write256`):** For high-frequency workloads where multiple items are produced in a tight loop, this library's `WriteTransaction` API provides the highest performance. By amortizing the cost of atomic synchronization over a large number of fast, non-atomic `try_push` calls, it achieves a throughput of **`~923 M/s`**, more than double that of Moodycamel's single-item API.

*   **3. Batch Transfers (`Batch`):** This library's `try_write` API, designed for transferring pre-prepared blocks of data, demonstrates strong scaling performance as the batch size increases. The data shows a clear crossover point: while Moodycamel is faster for very small batches (e.g., size 4), this library's throughput surpasses it significantly for batches of approximately **8-16 items or more**, reaching over **`1.1 G/s`** at a batch size of 256. In contrast, the `moodycamel::ReaderWriterQueue` (v1.0.7) API does not offer a non-blocking bulk enqueue method, and its performance remains flat at its single-item throughput rate.

### Conclusion

The data shows that this library is highly specialized for batch-oriented and high-frequency transactional workloads. While other libraries may offer higher performance for pure single-item transfers due to their architectural design, this queue's specialized APIs provide a significant throughput advantage in its intended use cases.

The recommended usage patterns are:
*   For producers generating many items in a tight loop, the **`WriteTransaction`** API offers the best performance.
*   For transferring pre-prepared blocks of data, the **`try_write`** lambda API is the most efficient method, especially for batch sizes of 16 or more.

