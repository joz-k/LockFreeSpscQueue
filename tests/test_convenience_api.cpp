#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <atomic>
#include <numeric>
#include <memory>

#include "LockFreeSpscQueue.h"

using DataType = long long;
using MyQueue  = LockFreeSpscQueue<DataType>;

// This producer uses the try_write convenience method.
void producer_thread(MyQueue& queue, size_t num_items_to_produce)
{
    std::mt19937 random_generator(std::random_device{}());
    std::uniform_int_distribution<size_t> batch_size_dist(1, 256);
    size_t items_produced_so_far = 0;

    while (items_produced_so_far < num_items_to_produce) {
        size_t desired_batch_size = std::min(batch_size_dist(random_generator),
                                             num_items_to_produce - items_produced_so_far);
        std::vector<DataType> local_batch(desired_batch_size);
        std::iota(local_batch.begin(), local_batch.end(), items_produced_so_far);

        size_t items_written_from_batch = 0;

        while (items_written_from_batch < local_batch.size()) {
            std::span<const DataType> sub_batch_to_write(
                local_batch.data() + items_written_from_batch,
                local_batch.size() - items_written_from_batch
            );

            // Use the convenience API. The logic is encapsulated in a lambda.
            size_t items_actually_written = queue.try_write(sub_batch_to_write.size(),
                [&](auto block1, auto block2)
                {
                    std::copy_n(sub_batch_to_write.begin(), block1.size(), block1.begin());
                    if (!block2.empty()) {
                        std::copy_n(sub_batch_to_write.begin() + block1.size(),
                                    block2.size(),
                                    block2.begin());
                    }
                });

            if (items_actually_written > 0) {
                items_written_from_batch += items_actually_written;
            } else {
                std::this_thread::yield();
            }
        }

        items_produced_so_far += local_batch.size();
    }
}

// This consumer uses the try_read convenience method.
void consumer_thread(MyQueue& queue,
                     std::atomic<bool>& producer_is_done,
                     std::atomic<bool>& test_failed)
{
    std::mt19937 random_generator(std::random_device{}());
    std::uniform_int_distribution<size_t> batch_size_dist(1, 512);
    DataType next_expected_value = 0;

    while (true) {
        // Use the convenience API. The verification logic is encapsulated in a lambda.
        // We need to capture by reference to modify next_expected_value and test_failed.
        [[maybe_unused]] size_t items_read = queue.try_read(batch_size_dist(random_generator),
            [&](auto block1, auto block2)
            {
                for (const auto& received_value : block1) {
                    if (received_value != next_expected_value++) { test_failed.store(true); }
                }

                for (const auto& received_value : block2) {
                    if (received_value != next_expected_value++) { test_failed.store(true); }
                }
            });
 
        if (test_failed.load()) { return; }

        // Exit condition check remains the same.
        if (producer_is_done.load(std::memory_order_acquire)) {
            if (queue.get_num_items_ready() == 0) break;
        }
    }
}


int main()
{
    const size_t TOTAL_ITEMS_TO_PROCESS = 500'000;
    const size_t QUEUE_CAPACITY = 8192;
    std::cout << "Running Test: Convenience API (try_write/try_read)...\n";

    auto shared_data_buffer = std::make_unique<std::vector<DataType>>(QUEUE_CAPACITY);
    MyQueue queue(*shared_data_buffer);

    std::atomic<bool> producer_is_done = false;
    std::atomic<bool> test_failed = false;

    std::jthread producer(producer_thread, std::ref(queue), TOTAL_ITEMS_TO_PROCESS);
    std::jthread consumer(consumer_thread,
                          std::ref(queue),
                          std::ref(producer_is_done),
                          std::ref(test_failed));

    producer.join();
    producer_is_done.store(true, std::memory_order_release);
    consumer.join();

    if (test_failed.load()) {
        std::cout << "Test FAILED.\n"; return 1;
    }

    if (queue.get_num_items_ready() != 0) {
        std::cout << "Test FAILED: Queue not empty.\n"; return 1;
    }

    std::cout << "Test PASSED.\n";
    return 0;
}

