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
            std::span<const DataType> sub_batch_to_write(local_batch.data()
                                                            + items_written_from_batch,
                                                         local_batch.size()
                                                            - items_written_from_batch);
            auto write_scope = queue.prepare_write(sub_batch_to_write.size());
            const size_t items_we_can_write = write_scope.get_items_written();

            if (items_we_can_write > 0) {
                auto block1 = write_scope.get_block1();
                auto block2 = write_scope.get_block2();
                std::copy_n(sub_batch_to_write.begin(), block1.size(), block1.begin());
                if (!block2.empty()) {
                    std::copy_n(sub_batch_to_write.begin() + block1.size(),
                                block2.size(),
                                block2.begin());
                }
                items_written_from_batch += items_we_can_write;
            } else {
                std::this_thread::yield();
            }
        }
        items_produced_so_far += local_batch.size();
    }
}

void consumer_thread(MyQueue& queue,
                     std::atomic<bool>& producer_is_done,
                     std::atomic<bool>& test_failed)
{
    std::mt19937 random_generator(std::random_device{}());
    std::uniform_int_distribution<size_t> batch_size_dist(1, 512);
    DataType next_expected_value = 0;

    while (true) {
        auto read_scope = queue.prepare_read(batch_size_dist(random_generator));
        if (read_scope.get_items_read() > 0) {
            for (const auto& received_value : read_scope.get_block1()) {
                if (received_value != next_expected_value) {
                    std::cerr << "Batch Test FAILED! Expected: " << next_expected_value
                              << ", Got: " << received_value << "\n";
                    test_failed.store(true);
                    return;
                }
                next_expected_value++;
            }
            for (const auto& received_value : read_scope.get_block2()) {
                if (received_value != next_expected_value) {
                    std::cerr << "Batch Test FAILED! Expected: " << next_expected_value
                              << ", Got: " << received_value << "\n";
                    test_failed.store(true);
                    return;
                }
                next_expected_value++;
            }
        } else {
            if (producer_is_done.load(std::memory_order_acquire)) {
                if (queue.get_num_items_ready() == 0) break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}


int main()
{
    const size_t TOTAL_ITEMS_TO_PROCESS = 500'000;
    const size_t QUEUE_CAPACITY = 8192;

    std::cout << "Running Test: Batch Transfer...\n";

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
        std::cout << "Test FAILED.\n";
        return 1;
    }

    if (queue.get_num_items_ready() != 0) {
        std::cout << "Test FAILED: Queue not empty at end.\n";
        return 1;
    }

    std::cout << "Test PASSED.\n";
    return 0;
}

