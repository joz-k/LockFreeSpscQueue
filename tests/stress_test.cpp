#include <iostream>
#include <vector>
#include <iterator>
#include <thread>
#include <random>
#include <chrono>

#include "LockFreeSpscQueue.h"

// Helper `assert` which aborts also in the release builds
#define always_assert(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "Assertion failed: " << #condition \
                      << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while(0)

// Stress test with concurrent producer and consumer
void stress_test()
{
    std::cout << "\n=== Stress Test ===\n";
    const size_t TOTAL_PRODUCER_WRITES = 100'000;
    const size_t MAX_ITEMS_TO_WRITE = 4;
    std::vector<int> buffer(4); // Small buffer to force wrap-around
    LockFreeSpscQueue<int> queue(buffer);
    std::vector<int> produced_data, consumed_data;
    produced_data.reserve(TOTAL_PRODUCER_WRITES * MAX_ITEMS_TO_WRITE);
    consumed_data.reserve(TOTAL_PRODUCER_WRITES * MAX_ITEMS_TO_WRITE);
    std::atomic<bool> producer_done{false}; // Flag to signal producer completion
    const auto start_time = std::chrono::steady_clock::now();

    // Producer thread: Write random-sized chunks
    auto producer = [&]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> size_dist(1, MAX_ITEMS_TO_WRITE);
        std::vector<int> data_buffer(MAX_ITEMS_TO_WRITE);
        std::uniform_int_distribution<int> value_dist(1, 100);
        size_t total_written = 0;

        for (int i = 0; i < TOTAL_PRODUCER_WRITES; ++i) {
            size_t to_write = size_dist(gen);
            auto write_scope = queue.prepare_write(to_write);
            size_t items_to_write = write_scope.get_items_written();

            if (items_to_write == 0) {
                std::this_thread::yield();
                continue;
            }

            auto data = std::span{data_buffer.begin(), items_to_write};
            for (auto& item : data) {
                item = value_dist(gen);
            }

            std::copy_n(data.begin(),
                        write_scope.get_block1().size(),
                        write_scope.get_block1().begin());

            if (write_scope.get_block2().size() > 0) {
                std::copy_n(data.begin() + write_scope.get_block1().size(),
                            write_scope.get_block2().size(),
                            write_scope.get_block2().begin());
            }

            // Collect produced data for verification
            produced_data.insert(produced_data.end(), data.begin(), data.end());
            total_written += items_to_write;

            std::this_thread::yield();
        }
        producer_done.store(true, std::memory_order_release); // Signal completion
        std::cout << "Producer: Finished, total written: " << total_written << "\n";
    };

    // Consumer thread: Read random-sized chunks
    auto consumer = [&]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> size_dist(1, 4);
        size_t total_read = 0;

        while (true) {
            // Check for timeout (e.g., 10 seconds)
            if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(10)) {
                std::cout << "Consumer: Timeout reached, exiting\n";
                break;
            }

            // Exit if producer is done and queue is empty
            if (   producer_done.load(std::memory_order_acquire)
                && queue.get_num_items_ready() == 0)
            {
                std::cout << "Consumer: Producer done and queue empty, exiting\n";
                break;
            }

            size_t to_read = size_dist(gen);
            auto read_scope = queue.prepare_read(to_read);
            size_t items_to_read = read_scope.get_items_read();

            if (items_to_read == 0) {
                std::this_thread::yield();
                continue;
            }

            // Collect consumed data for verification
            std::copy(read_scope.get_block1().begin(),
                      read_scope.get_block1().end(),
                      std::inserter(consumed_data, consumed_data.end()));

            if (read_scope.get_block2().size() > 0) {
                std::copy(read_scope.get_block2().begin(),
                          read_scope.get_block2().end(),
                          std::inserter(consumed_data, consumed_data.end()));
            }

            total_read += items_to_read;

            std::this_thread::yield();
        }

        std::cout << "Consumer: Finished, total read: " << total_read << "\n";
    };

    // Run producer and consumer concurrently
    std::thread producer_thread(producer);
    std::thread consumer_thread(consumer);
    producer_thread.join();
    consumer_thread.join();

    // Verify that consumed data matches produced data
    always_assert(produced_data.size() == consumed_data.size());
    always_assert(produced_data == consumed_data);
    std::cout << "Stress test passed: Produced and consumed data match\n";
}

int main()
{
    stress_test();
    return 0;
}

