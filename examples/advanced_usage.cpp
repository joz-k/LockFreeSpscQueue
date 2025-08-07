#include <iostream>
#include <vector>
#include <thread>
#include <random>
#include <string>
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

// Utility to print a span for debugging
template <typename SpanType>
void print_span(SpanType&& s, const std::string& label) {
    std::cout << label << ": [ ";
    for (const auto& item : s) {
        std::cout << item << " ";
    }
    std::cout << "]\n";
}

// Basic usage example
void basic_usage()
{
    std::cout << "\n=== Basic Usage Example ===\n";
    std::vector<int> buffer(8); // Power-of-two buffer
    LockFreeSpscQueue<int> queue(buffer);

    // Producer: Write 5 items
    std::vector<int> data = {1, 2, 3, 4, 5};

    {
        auto write_scope = queue.prepare_write(5);
        std::cout << "Producer: Preparing to write " << write_scope.get_items_written()
                  << " items\n";

        // Copy data first
        std::copy_n(data.begin(),
                    write_scope.get_block1().size(),
                    write_scope.get_block1().begin());

        // An above `copy_n` is equivalent to:
        //     std::copy(data.begin(),
        //               data.begin() + write_scope.get_block1().size(),
        //               write_scope.get_block1().begin());

        if (write_scope.get_block2().size() > 0) {
            std::copy_n(data.begin() + write_scope.get_block1().size(),
                        write_scope.get_block2().size(),
                        write_scope.get_block2().begin());

            // An above `copy_n` is equivalent to:
            //      std::copy(data.begin() + write_scope.get_block1().size(),
            //                data.begin() + write_scope.get_block1().size()
            //                             + write_scope.get_block2().size(),
            //                write_scope.get_block2().begin());
        }

        // Print spans after writing to show the actual data
        print_span(write_scope.get_block1(), "Block 1 (after write)");
        print_span(write_scope.get_block2(), "Block 2 (after write)");
        // RAII commits automatically
    }

    // Consumer: Read 5 items
    {
        auto read_scope = queue.prepare_read(5);
        std::cout << "Consumer: Preparing to read " << read_scope.get_items_read() << " items\n";
        print_span(read_scope.get_block1(), "Block 1");
        print_span(read_scope.get_block2(), "Block 2");

        std::vector<int> result(read_scope.get_items_read());
        std::copy(read_scope.get_block1().begin(),
                  read_scope.get_block1().end(),
                  result.begin());

        if (read_scope.get_block2().size() > 0) {
            std::copy(read_scope.get_block2().begin(),
                      read_scope.get_block2().end(),
                      result.begin() + read_scope.get_block1().size());
        }

        // Verify
        always_assert(result == data);
        std::cout << "Consumer: Read data matches input\n";
    }
}

// Stress test with concurrent producer and consumer
void stress_test()
{
    std::cout << "\n=== Stress Test ===\n";
    std::vector<int> buffer(4); // Small buffer to force wrap-around
    LockFreeSpscQueue<int> queue(buffer);
    std::vector<int> produced_data, consumed_data;
    std::atomic<bool> producer_done{false}; // Flag to signal producer completion
    const auto start_time = std::chrono::steady_clock::now();

    // Producer thread: Write random-sized chunks
    auto producer = [&]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> size_dist(1, 4);
        std::uniform_int_distribution<int> value_dist(1, 100);
        size_t total_written = 0;

        for (int i = 0; i < 100'000; ++i) {
            size_t to_write = size_dist(gen);
            auto write_scope = queue.prepare_write(to_write);
            size_t items_to_write = write_scope.get_items_written();

            if (items_to_write == 0) {
                std::this_thread::yield();
                continue;
            }

            std::vector<int> data(items_to_write);
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
            std::cout << "Producer: Wrote " << items_to_write
                      << " items (total: " << total_written << ")\n";
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
            // Check for timeout (e.g., 5 seconds) to prevent infinite loops
            if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(5)) {
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

            std::vector<int> data(items_to_read);
            std::copy(read_scope.get_block1().begin(),
                      read_scope.get_block1().end(),
                      data.begin());

            if (read_scope.get_block2().size() > 0) {
                std::copy(read_scope.get_block2().begin(),
                          read_scope.get_block2().end(),
                          data.begin() + read_scope.get_block1().size());
            }

            // Collect consumed data for verification
            consumed_data.insert(consumed_data.end(), data.begin(), data.end());
            total_read += items_to_read;
            std::cout << "Consumer: Read " << items_to_read
                      << " items (total: " << total_read << ")\n";
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

// Edge case test: Full and empty queue
void edge_case_test()
{
    std::cout << "\n=== Edge Case Test ===\n";
    std::vector<int> buffer(4);
    LockFreeSpscQueue<int> queue(buffer);

    // Fill the queue
    {
        auto write_scope = queue.prepare_write(4);
        always_assert(write_scope.get_items_written() == 4);
        std::fill(write_scope.get_block1().begin(), write_scope.get_block1().end(), 42);
        if (write_scope.get_block2().size() > 0) {
            std::fill(write_scope.get_block2().begin(), write_scope.get_block2().end(), 42);
        }
        std::cout << "Edge case: Filled queue with 4 items\n";
    }

    // Try to write to a full queue
    {
        auto write_scope = queue.prepare_write(1);
        always_assert(write_scope.get_items_written() == 0);
        std::cout << "Edge case: Write to full queue returned 0 items\n";
    }

    // Read all items
    {
        auto read_scope = queue.prepare_read(4);
        always_assert(read_scope.get_items_read() == 4);

        std::vector<int> result(read_scope.get_items_read());
        std::copy(read_scope.get_block1().begin(),
                  read_scope.get_block1().end(),
                  result.begin());

        if (read_scope.get_block2().size() > 0) {
            std::copy(read_scope.get_block2().begin(),
                      read_scope.get_block2().end(),
                      result.begin() + read_scope.get_block1().size());
        }

        always_assert(std::all_of(result.begin(), result.end(), [](int x) { return x == 42; }));
        std::cout << "Edge case: Read 4 items from full queue\n";
    }

    // Try to read from an empty queue
    {
        auto read_scope = queue.prepare_read(1);
        always_assert(read_scope.get_items_read() == 0);
        std::cout << "Edge case: Read from empty queue returned 0 items\n";
    }
}

int main()
{
    basic_usage();
    stress_test();
    edge_case_test();
    std::cout << "\nAll tests passed!\n";
    return 0;
}

