# Examples

This document provides a "cookbook" of common usage patterns for the `LockFreeSpscQueue`.

## 1. Initialization

The `LockFreeSpscQueue` is an index manager that operates on a user-provided memory buffer. The user is responsible for creating and owning the buffer and providing a `std::span` of it to the queue's constructor.

The buffer's capacity **must** be a power of two.

```cpp
#include "LockFreeSpscQueue.h"
#include <vector>
#include <string>

// Define a data type to be used in the queue.
struct Message {
    int id;
    std::string payload;
};

// Define the capacity.
const size_t QUEUE_CAPACITY = 128; // 2^7

// Create the memory buffer that will be shared between threads.
std::vector<Message> shared_data_buffer(QUEUE_CAPACITY);

// Create the queue, giving it a non-owning view of our buffer.
LockFreeSpscQueue<Message> queue(shared_data_buffer);
```
**Lifetime Note:** The user must ensure that `shared_data_buffer` outlives the `queue` object.

---

## 2. Writing to the Queue (Producer)

There are three primary ways to write to the queue, each suited for a different use case.

### High-Frequency Writes: `WriteTransaction`

This is the **highest-performance method** for producers that generate many individual items in a tight loop. It amortizes the cost of atomic operations over many fast, non-atomic pushes.

```cpp
void high_frequency_producer(LockFreeSpscQueue<Message>& queue)
{
    for (int i = 0; i < 100; ) {
        // 1. Try to start a transaction for a batch of up to 16 items.
        //    This returns an optional; it will be empty if the queue is too full.
        if (auto transaction = queue.try_start_write(16)) {
            // 2. We got a reservation! Push items into it.
            //    This `try_push` is extremely fast (non-atomic).
            while (i < 100 && transaction->try_push({i, "some data"})) {
                // Item was successfully pushed into the transaction's reserved space.
                i++;
            }
            // 3. The transaction automatically commits the items that were
            //    pushed when it goes out of scope here.
        } else {
            // Queue was too full to even start a transaction, yield to the consumer.
            std::this_thread::yield();
        }
    }
}
```
*Move semantics are also supported: `transaction->try_push(std::move(my_message));`*

### Batch Writes (Convenience): `try_write`

This is the **recommended method for transferring a pre-prepared batch of data**. It's safe, convenient, and highly efficient. It accepts a lambda that is only called if space is available.

```cpp
void batch_producer(LockFreeSpscQueue<Message>& queue)
{
    // Prepare a local batch of data to send.
    std::vector<Message> local_batch = { {0, "a"}, {1, "b"}, {2, "c"} };

    size_t items_written = 0;
    while (items_written < local_batch.size()) {
        // Create a view of the remaining items we need to write.
        std::span<const Message> sub_batch(local_batch.data() + items_written,
                                           local_batch.size() - items_written);

        // Ask `try_write` to write the sub-batch. It will write as many items as
        // it can and return the count. The lambda handles the actual copy.
        items_written += queue.try_write(sub_batch.size(), [&](auto block1, auto block2) {
            std::copy_n(sub_batch.begin(), block1.size(), block1.begin());
            if (!block2.empty()) {
                std::copy_n(sub_batch.begin() + block1.size(), block2.size(), block2.begin());
            }
        });

        // If the queue was full, items_written won't increase, and we'll loop.
    }
}
```

### Batch Writes (Low-Level): `prepare_write`

This is the low-level "engine" that the other write methods are built upon. It provides maximum control by returning a `WriteScope` object that grants direct `std::span` access to the underlying buffer.

```cpp
void low_level_batch_producer(LockFreeSpscQueue<Message>& queue,
                              const std::vector<Message>& data)
{
    // Ask to reserve up to `data.size()` items in the queue for writing.
    auto write_scope = queue.prepare_write(data.size());

    // `get_items_written()` returns the actual number of slots reserved,
    // which may be less than what we asked for if the queue was partially full.
    size_t items_to_write = write_scope.get_items_written();

    if (items_to_write > 0) {
        auto block1 = write_scope.get_block1();
        auto block2 = write_scope.get_block2();

        // Copy data into the contiguous memory blocks.
        std::copy_n(data.begin(), block1.size(), block1.begin());
        if (!block2.empty()) {
            std::copy_n(data.begin() + block1.size(), block2.size(), block2.begin());
        }
    }
    // The write is automatically committed when `write_scope` is destroyed.
}
```

---

## 3. Reading from the Queue (Consumer)

There are two primary ways to read from the queue.

### Batch Reads (Convenience): `try_read`

This is the **recommended and most efficient method** for consuming data. It accepts a lambda which is given direct `std::span` access to the largest available contiguous blocks of readable data.

```cpp
void batch_consumer(LockFreeSpscQueue<Message>& queue)
{
    // Ask to read up to 16 items at a time.
    const size_t items_read = queue.try_read(16, [&](auto block1, auto block2) {
        // Process all items in the first contiguous block.
        for (const auto& msg : block1) {
            std::cout << "Consumer: Got ID " << msg.id << "\n";
        }
        // Process all items in the second (wrapped-around) block.
        for (const auto& msg : block2) {
            std::cout << "Consumer: Got ID " << msg.id << "\n";
        }
    });

    if (items_read == 0) {
        // The queue was empty.
        std::this_thread::yield();
    }
}
```

### Batch Reads (Low-Level): `prepare_read`

This is the low-level counterpart to `prepare_write`. It gives you a `ReadScope` object that provides direct access to the readable memory blocks.

```cpp
void low_level_batch_consumer(LockFreeSpscQueue<Message>& queue)
{
    std::vector<Message> consumed_data;

    // Ask to read up to 16 items from the queue.
    auto read_scope = queue.prepare_read(16);

    if (read_scope.get_items_read() > 0)
    {
        // Copy the data from the queue's buffer into our local vector.
        auto block1 = read_scope.get_block1();
        consumed_data.insert(consumed_data.end(), block1.begin(), block1.end());

        auto block2 = read_scope.get_block2();
        if (!block2.empty()) {
            consumed_data.insert(consumed_data.end(), block2.begin(), block2.end());
        }
    }
    // The read is automatically committed when `read_scope` is destroyed.
}
```
