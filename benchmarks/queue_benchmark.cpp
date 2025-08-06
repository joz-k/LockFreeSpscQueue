#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include "LockFreeSpscQueue.h"

using DataType = long long;

// This is the function that Google Benchmark will run repeatedly.
// It measures the throughput of transferring items from a producer to a consumer.
static void BM_QueueThroughput(benchmark::State& state)
{
    // The size of the batch to transfer in one go. This is passed from the ->Arg() call.
    const size_t batch_size = state.range(0);
    const size_t queue_capacity = 8192;

    // We set up the queue for each run to ensure a clean state.
    std::vector<DataType> shared_buffer(queue_capacity);
    LockFreeSpscQueue<DataType> queue(shared_buffer);

    // Atomic flag to signal the consumer thread to stop.
    std::atomic<bool> done = false;

    // The consumer thread's logic.
    std::jthread consumer([&](std::stop_token st)
    {
        while (!st.stop_requested()) {
            // Read in batches for maximum efficiency.
            auto read_scope = queue.prepare_read(batch_size);
            if (read_scope.get_items_read() == 0) {
                // If the queue is empty, yield.
                std::this_thread::yield();
            }
        }
        // Drain any remaining items after the producer is done.
        while (queue.get_num_items_ready() > 0) {
            [[maybe_unused]] auto scope = queue.prepare_read(queue_capacity);
        }
    });

    // The main benchmark loop. Google Benchmark controls this loop.
    for (auto _ : state) {
        // Inside this loop, we, the producer, will push a fixed number of items.
        // The total time for this loop is what's measured.
        size_t items_produced = 0;
        while (items_produced < queue_capacity) {
            auto write_scope = queue.prepare_write(batch_size);
            const size_t items_written = write_scope.get_items_written();
            if (items_written > 0) {
                items_produced += items_written;
            } else {
                // Queue is full, yield to the consumer.
                std::this_thread::yield();
            }
        }
    }

    // Clean up the consumer thread.
    consumer.request_stop();

    // --- Report Metrics ---
    // Tell Google Benchmark how many items we processed in total during the benchmark.
    // This is the number of iterations times the number of items per iteration.
    state.SetItemsProcessed(state.iterations() * queue_capacity);

    // Report throughput in items per second.
    state.counters["Items/s"] = benchmark::Counter(state.items_processed(),
                                                   benchmark::Counter::kIsRate);
}

// Register the benchmark function to be run.
// We will test it with different batch sizes to see how performance scales.
BENCHMARK(BM_QueueThroughput)
    ->Arg(1)        // Test with single-item transfers
    ->Arg(4)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)      // Test with large batch transfers
    ->Unit(benchmark::kMillisecond); // Display results in milliseconds.

// The main function that runs all benchmarks.
BENCHMARK_MAIN();

