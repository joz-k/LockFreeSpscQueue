#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <atomic>
#include <memory>

#include "LockFreeSpscQueue.h"

using DataType = long long;
using MyQueue  = LockFreeSpscQueue<DataType>;

void producer_thread(MyQueue& queue, size_t num_items_to_produce)
{
    std::mt19937 random_generator(std::random_device{}());
    std::uniform_int_distribution<int> sleep_dist(0, 20);

    for (size_t i = 0; i < num_items_to_produce; ++i) {
        DataType item_to_write = static_cast<DataType>(i);
        while (true) {
            auto write_scope = queue.prepare_write(1);
            if (write_scope.get_items_written() == 1) {
                // Get a span to the writable block.
                auto block1 = write_scope.get_block1();
                block1[0] = item_to_write;
                break;
            }
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_dist(random_generator)));
    }
}

void consumer_thread(MyQueue& queue,
                     std::atomic<bool>& producer_is_done,
                     std::atomic<bool>& test_failed)
{
    DataType next_expected_value = 0;

    while (true) {
        auto read_scope = queue.prepare_read(1);

        if (read_scope.get_items_read() == 1) {
            // Get a span to the readable block.
            auto block1 = read_scope.get_block1();
            const DataType received_value = block1[0];

            if (received_value != next_expected_value) {
                std::cerr << "Single Item Test FAILED! Expected: " << next_expected_value
                          << ", Got: " << received_value << "\n";
                test_failed.store(true);
                return;
            }
            next_expected_value++;
        } else {
            if (producer_is_done.load(std::memory_order_acquire)) {
                if (queue.get_num_items_ready() == 0) {
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }
}

int main()
{
    const size_t TOTAL_ITEMS_TO_PROCESS = 100'000;
    const size_t QUEUE_CAPACITY = 4096;

    std::cout << "Running Test: Single Item Transfer...\n";

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

    if (test_failed.load())
    {
        std::cout << "Test FAILED.\n";
        return 1;
    }

    if (queue.get_num_items_ready() != 0)
    {
        std::cout << "Test FAILED: Queue not empty at end.\n";
        return 1;
    }

    std::cout << "Test PASSED.\n";
    return 0;
}

