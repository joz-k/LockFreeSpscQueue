#include "LockFreeSpscQueue.h"

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <numeric>   // For std::iota
#include <algorithm> // For std::ranges::copy

// This example demonstrates the `std::ranges` integration of the WriteScope
// and ReadScope objects. It shows how to use modern, idiomatic C++ to interact
// with the queue, abstracting away the underlying two-block nature of the
// circular buffer.

// --- Producer Thread ---
void producer_thread(LockFreeSpscQueue<int>& queue)
{
    std::cout << "Producer: Starting...\n";

    // --- Method 1: Writing with a range-based for loop ---
    std::cout << "Producer: Preparing to write items 0-9 using a for loop...\n";
    while (true) {
        auto write_scope = queue.prepare_write(10);
        // Check if the scope is valid by checking the number of items.
        if (write_scope.get_items_written() > 0)
        {
            int i = 0;
            for (int& slot : write_scope) {
                slot = i++;
            }
            std::cout << "Producer:   Wrote " << write_scope.get_items_written() << " items.\n";
            break;
        }
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // --- Method 2: Batch Writing with a std::ranges algorithm ---
    std::cout << "Producer: Preparing to write items 100-115 using std::ranges::copy...\n";
    std::vector<int> source_data(16);
    std::iota(source_data.begin(), source_data.end(), 100);

    size_t total_written = 0;
    while (total_written < source_data.size())
    {
        auto write_scope = queue.prepare_write(source_data.size() - total_written);
        // Check if the scope is valid.
        if (write_scope.get_items_written() > 0)
        {
            const size_t can_write_count = write_scope.get_items_written();
            std::span<const int> source_sub_batch(source_data.data() + total_written,
                                                  can_write_count);
            std::ranges::copy(source_sub_batch, write_scope.begin());
            total_written += can_write_count;
            std::cout << "Producer:   Copied " << can_write_count << " items. ("
                      << total_written << "/" << source_data.size() << " total)\n";
        }
        else {
            std::this_thread::yield();
        }
    }

    std::cout << "Producer: Finished.\n";
}

// --- Consumer Thread ---
void consumer_thread(LockFreeSpscQueue<int>& queue, std::atomic<bool>& producer_is_done)
{
    std::cout << "Consumer: Waiting for items...\n";
    while (true)
    {
        auto read_scope = queue.prepare_read(32);
        // Check if the scope is valid.
        if (read_scope.get_items_read() > 0)
        {
            std::cout << "Consumer: Reading a batch of " << read_scope.get_items_read() << " items...\n";
            for (const int& item : read_scope) {
                std::cout << "Consumer:   Got " << item << "\n";
            }
        }
        else
        {
            if (producer_is_done.load(std::memory_order_acquire)) {
                if (queue.get_num_items_ready() == 0) {
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    std::cout << "Consumer: Finished.\n";
}


int main()
{
    const size_t QUEUE_CAPACITY = 128;
    std::vector<int> shared_data_buffer(QUEUE_CAPACITY);
    LockFreeSpscQueue<int> queue(shared_data_buffer);
    std::atomic<bool> producer_is_done = false;

    std::jthread producer(producer_thread, std::ref(queue));
    std::jthread consumer(consumer_thread, std::ref(queue), std::ref(producer_is_done));

    producer.join();
    producer_is_done.store(true, std::memory_order_release);
}

