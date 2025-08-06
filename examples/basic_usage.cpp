#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <string>

#include "LockFreeSpscQueue.h"

struct Message
{
    long long message_id;
    std::string text;
};

// --- Producer Thread ---
// Generates a few messages and sends them to the consumer.
void producer_thread(LockFreeSpscQueue<Message>& queue)
{
    std::cout << "Producer: Starting up...\n";
    for (int i = 0; i < 5; ++i)
    {
        Message msg = { i, "Hello from message " + std::to_string(i) };

        // Keep trying to write until it succeeds.
        // `try_write` returns 0 if the queue is full.
        while (queue.try_write(1, [&](auto block1, auto){ block1[0] = std::move(msg); }) == 0)
        {
            // Queue was full, yield to the consumer and try again.
            std::this_thread::yield();
        }

        std::cout << "Producer: Sent message " << i << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Producer: All messages sent. Shutting down.\n";
}


// --- Consumer Thread ---
// Waits for messages and prints them.
void consumer_thread(LockFreeSpscQueue<Message>& queue, std::atomic<bool>& producer_is_done)
{
    std::cout << "Consumer: Starting up and waiting for messages...\n";
    while (true)
    {
        // Capture the return value. If it's 0, no items were read.
        const size_t items_read = queue.try_read(1, [&](auto block1, auto)
        {
            Message received_msg = std::move(block1[0]);
            std::cout << "Consumer: Received message! ID: " << received_msg.message_id
                      << ", Text: \"" << received_msg.text << "\"\n";
        });

        if (items_read == 0)
        {
            // Queue was empty. Check if we should exit.
            if (producer_is_done.load(std::memory_order_acquire))
            {
                if (queue.get_num_items_ready() == 0) break;
            }
        }
    }
     std::cout << "Consumer: Shutting down.\n";
}


int main()
{
    // 1. Define the capacity for our queue. MUST be a power of two.
    const size_t QUEUE_CAPACITY = 16;

    // 2. Create the data buffer that will be shared between threads.
    //    We use a vector here for easy memory management.
    std::vector<Message> shared_data_buffer(QUEUE_CAPACITY);

    // 3. Create the queue manager. We pass it a `std::span` that gives it a
    //    non-owning view of our shared buffer.
    LockFreeSpscQueue<Message> queue(shared_data_buffer);

    // 4. Set up synchronization flags.
    std::atomic<bool> producer_is_done = false;

    // 5. Start the threads. std::jthread automatically joins on scope exit.
    std::jthread producer(producer_thread, std::ref(queue));
    std::jthread consumer(consumer_thread, std::ref(queue), std::ref(producer_is_done));

    // 6. Wait for the producer to finish its work.
    producer.join();
    producer_is_done.store(true, std::memory_order_release);

    // `consumer.join()` will be called automatically by the jthread destructor.
    // The program will wait here until the consumer has finished.
}
