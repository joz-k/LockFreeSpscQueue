#include "LockFreeSpscQueue.h"
#include "readerwriterqueue.h" // Moodycamel's queue header
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include <numeric>
#include <random>

using DataType = int64_t;
constexpr size_t RandomDataSize    = 4001; // The prime to make patterns less obvious
constexpr size_t ItemsPerIteration = 100'025;
constexpr size_t QueueCapacity     = 65536; // 2^16

// Shared Test Data Generation
const std::vector<DataType>& get_random_data() {
    static const auto data = []{
        std::vector<DataType> d(RandomDataSize);
        std::mt19937_64 rng(33317); // Fixed seed for test stability
        std::uniform_int_distribution<DataType> dist;
        for (auto& val : d) { val = dist(rng); }
        return d;
    }();
    return data;
}

// Benchmark Group 1A: Single Item

static void BM_ThisQueue_SingleItem(benchmark::State& state) {
    const auto& random_data = get_random_data();
    std::vector<DataType> shared_buffer(QueueCapacity);
    LockFreeSpscQueue<DataType> queue(shared_buffer);

    std::atomic<bool> verification_failed  = false;
    std::atomic<bool> consumer_should_stop = false;
    std::jthread consumer([&] {
        size_t i = 0;
        while (!(   consumer_should_stop.load(std::memory_order_relaxed)
                 && queue.get_num_items_ready() == 0))
        {
            auto scope = queue.prepare_read(1);
            if (scope.get_items_read() == 1) {
                if (scope.get_block1()[0] != random_data[i % RandomDataSize]) verification_failed.store(true);
                i++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    size_t total_written = 0;
    for (auto _ : state) {
        if (verification_failed.load(std::memory_order_relaxed)) {
            state.SkipWithError("Verification failed!"); return;
        }
        for (size_t n = 0; n < ItemsPerIteration; ++n) {
            const auto& item_to_write = random_data[total_written % RandomDataSize];
            while (true) {
                auto scope = queue.prepare_write(1);
                if (scope.get_items_written() == 1) {
                    scope.get_block1()[0] = item_to_write;
                    break;
                }
            }
            total_written++;
        }
    }

    consumer_should_stop.store(true, std::memory_order_relaxed);
    state.SetItemsProcessed(total_written);
}
BENCHMARK(BM_ThisQueue_SingleItem)->Unit(benchmark::kMillisecond)->UseRealTime();


// Benchmark Group 1B: Single Item, "Transaction" Builk-Write API

static void BM_ThisQueue_SingleItem_Write256(benchmark::State& state) {
    const auto& random_data = get_random_data();
    std::vector<DataType> shared_buffer(QueueCapacity);
    LockFreeSpscQueue<DataType> queue(shared_buffer);

    std::atomic<bool> verification_failed  = false;
    std::atomic<bool> consumer_should_stop = false;

    std::jthread consumer([&]{
        size_t received_count = 0;
        while (!(   consumer_should_stop.load(std::memory_order_relaxed)
                 && queue.get_num_items_ready() == 0))
        {
            // Always read much items from the queue as they are ready
            const size_t items_read = queue.try_read(QueueCapacity, [&](auto b1, auto b2) {
                for (const auto& item : b1) {
                    if (item != random_data[(received_count++) % RandomDataSize]) {
                        verification_failed.store(true);
                    }
                }
                for (const auto& item : b2) {
                    if (item != random_data[(received_count++) % RandomDataSize]) {
                        verification_failed.store(true);
                    }
                }
            });
            if (items_read == 0) {
                std::this_thread::yield();
            }
        }
    });

    size_t total_written = 0;
    for (auto _ : state) {
        for (size_t n = 0; n < ItemsPerIteration; ) {
            // Start a transaction for up to 256 items at a time
            auto transaction = queue.try_start_write(256);
            if (transaction) {
                // Push a single item into transaction in a loop, until the transaction is full
                while(   n < ItemsPerIteration
                      && transaction->try_push(random_data[total_written % RandomDataSize]))
                {
                    total_written++;
                    n++;
                }
                // Transaction commits automatically when it goes out of scope here.
            }
        }
    }
    consumer_should_stop.store(true, std::memory_order_relaxed);
    state.SetItemsProcessed(total_written);
}
BENCHMARK(BM_ThisQueue_SingleItem_Write256)->Unit(benchmark::kMillisecond)->UseRealTime();


static void BM_Moodycamel_SingleItem(benchmark::State& state) {
    const auto& random_data = get_random_data();
    moodycamel::ReaderWriterQueue<DataType> queue(QueueCapacity);
    std::atomic<bool> verification_failed  = false;
    std::atomic<bool> consumer_should_stop = false;
    std::jthread consumer([&] {
        size_t i = 0;
        DataType item;
        while (!(   consumer_should_stop.load(std::memory_order_relaxed)
                 && queue.size_approx() == 0))
        {
            if (queue.try_dequeue(item)) {
                if (item != random_data[i % RandomDataSize]) verification_failed.store(true);
                i++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    size_t total_written = 0;
    for (auto _ : state) {
        for (size_t n = 0; n < ItemsPerIteration; ++n) {
            if (verification_failed.load(std::memory_order_relaxed)) {
                state.SkipWithError("Verification failed!"); return;
            }
            const auto& item_to_write = random_data[total_written % RandomDataSize];
            while (!queue.try_enqueue(item_to_write)) {}
            total_written++;
        }
    }
    consumer_should_stop.store(true, std::memory_order_relaxed);
    state.SetItemsProcessed(total_written);
}
BENCHMARK(BM_Moodycamel_SingleItem)->Unit(benchmark::kMillisecond)->UseRealTime();


// Benchmark Group 2: Batch/Bulk Transfers

static void BM_ThisQueue_Batch(benchmark::State& state) {
    const size_t batch_size = state.range(0);
    const auto& random_data = get_random_data();
    std::vector<DataType> shared_buffer(QueueCapacity);
    LockFreeSpscQueue<DataType> queue(shared_buffer);

    std::atomic<bool> verification_failed  = false;
    std::atomic<bool> consumer_should_stop = false;
    std::jthread consumer([&]{
        size_t received_count = 0;
        while (!(   consumer_should_stop.load(std::memory_order_relaxed)
                 && queue.get_num_items_ready() == 0))
        {
            const size_t items_read = queue.try_read(batch_size, [&](auto b1, auto b2) {
                // Read and verify block1
                for (const auto& item : b1) {
                    if (item != random_data[(received_count++) % RandomDataSize]) {
                        verification_failed.store(true);
                    }
                }
                // Read and verify block2
                for (const auto& item : b2) {
                    if (item != random_data[(received_count++) % RandomDataSize]) {
                        verification_failed.store(true);
                    }
                }
            });
            if (items_read == 0) {
                std::this_thread::yield();
            }
        }
    });

    size_t total_written = 0;
    for (auto _ : state) {
        for (size_t n = 0; n < ItemsPerIteration; ) {
            if (verification_failed.load(std::memory_order_relaxed)) {
                state.SkipWithError("Verification failed!");
                consumer_should_stop.store(true);
                return;
            }

            const size_t current_rand_idx  = total_written % RandomDataSize;
            const size_t items_to_rand_end = RandomDataSize - current_rand_idx;
            size_t remaining_in_iter  = ItemsPerIteration - n;
            size_t batch_to_send_size = std::min({batch_size,
                                                  remaining_in_iter,
                                                  items_to_rand_end});

            std::span<const DataType> sub_batch(&random_data[current_rand_idx], batch_to_send_size);

            size_t written_this_batch = 0;
            while (written_this_batch < sub_batch.size()) {
                written_this_batch += queue.try_write(sub_batch.size() - written_this_batch,
                                                      [&](auto b1, auto b2)
                {
                    // Copy block1
                    std::copy_n(sub_batch.begin() + written_this_batch, b1.size(), b1.begin());
                    if (!b2.empty()) {
                        // Copy block2 (if needed)
                        std::copy_n(sub_batch.begin() + written_this_batch + b1.size(),
                                    b2.size(),
                                    b2.begin());
                    }
                });
            }
            n += written_this_batch;
            total_written += written_this_batch;
        }
    }
    consumer_should_stop.store(true, std::memory_order_relaxed);
    state.SetItemsProcessed(total_written);
}
BENCHMARK(BM_ThisQueue_Batch)->Arg(4)->Arg(8)->Arg(16)->Arg(64)->Arg(256)->Unit(benchmark::kMillisecond)->UseRealTime();


static void BM_Moodycamel_Batch(benchmark::State& state) {
    const size_t batch_size = state.range(0);
    const auto& random_data = get_random_data();
    moodycamel::ReaderWriterQueue<DataType> queue(QueueCapacity);

    std::atomic<bool> verification_failed  = false;
    std::atomic<bool> consumer_should_stop = false;
    std::jthread consumer([&]{
        size_t received_count = 0;
        DataType item;
        while (!(   consumer_should_stop.load(std::memory_order_relaxed)
                 && queue.size_approx() == 0))
        {
            bool dequeued_something = false;
            for (size_t i = 0; i < batch_size; ++i) {
                if (queue.try_dequeue(item)) {
                    if (item != random_data[(received_count++) % RandomDataSize]) {
                        verification_failed.store(true);
                    }
                    dequeued_something = true;
                } else {
                    break;
                }
            }
            if (!dequeued_something) {
                std::this_thread::yield();
            }
        }
    });

    size_t total_written = 0;
    for (auto _ : state) {
        for (size_t n = 0; n < ItemsPerIteration; n += batch_size) {
            if (verification_failed.load(std::memory_order_relaxed)) {
                state.SkipWithError("Verification failed!");
                consumer_should_stop.store(true);
                return;
            }
            for (size_t i = 0; i < batch_size; ++i) {
                const auto& item_to_write = random_data[(total_written + i) % RandomDataSize];
                while (!queue.try_enqueue(item_to_write)) {}
            }
            total_written += batch_size;
        }
    }
    consumer_should_stop.store(true, std::memory_order_relaxed);
    state.SetItemsProcessed(total_written);
}
BENCHMARK(BM_Moodycamel_Batch)->Arg(4)->Arg(8)->Arg(16)->Arg(64)->Arg(256)->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
