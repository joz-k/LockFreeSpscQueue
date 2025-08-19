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

This is the **highest-performance API** for producers that generate many individual items in a tight loop. It amortizes the high cost of atomic synchronization over many fast, non-atomic pushes and emplaces.

The process involves three steps:
1.  Start a transaction for a batch of items using `try_start_write()`.
2.  If successful, use the transaction object's ultra-fast `try_emplace()` or `try_push()` methods to add items to the reserved space.
3.  The transaction automatically commits the items that were successfully added when its `WriteTransaction` object is destroyed.

#### The `try_emplace` Method (Recommended for Complex Objects)

`try_emplace` is the most efficient way to add a complex object to the queue. It constructs the object **in-place** directly in the queue's memory, avoiding any temporary objects or expensive move operations.

```cpp
#include <string>
#include <vector>

struct MyData {
    std::string s;
    std::vector<int> v;
    MyData(const std::string& str, size_t n) : s(str), v(n) {}
};

void emplace_producer(LockFreeSpscQueue<MyData>& queue)
{
    // Try to start a transaction for up to 16 items.
    if (auto transaction = queue.try_start_write(16))
    {
        // We got a reservation! Emplace items directly into it.
        // This is extremely fast, as there are no temporary MyData objects.
        transaction->try_emplace("hello", 100);
        transaction->try_emplace("world", 200);

        // 'transaction' automatically commits the 2 new items when it goes out of scope.
    }
}
```

#### The `try_push` Method (For Simple Types or Pre-existing Objects)

`try_push` is ideal for trivial types (like `int` or `float`) or when you already have an existing object that you want to move into the queue.

```cpp
void high_frequency_producer(LockFreeSpscQueue<Message>& queue)
{
    int next_item = 0;

    // Keep writing until we've sent 100 items.
    while (next_item < 100)
    {
        // 1. Try to start a transaction for a batch of up to 32 items.
        //    This returns an optional; it will be empty if the queue is too full.
        if (auto transaction = queue.try_start_write(32))
        {
            // 2. We got a reservation! Push items into it.
            //    Use try_push for this simple integer type.
            while (next_item < 100 && transaction->try_push(next_item)) {
                // Item was successfully pushed into the transaction's reserved space.
                next_item++;
            }
            // 3. The transaction automatically commits the items that were
            //    pushed when it goes out of scope here.
        } else {
            // The queue was too full to even start a transaction, yield to the consumer.
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
            // --- Copy Semantics ---
            // std::copy_n performs a copy-assignment for each element.
            std::copy_n(sub_batch.begin(), block1.size(), block1.begin());
            if (!block2.empty()) {
                std::copy_n(sub_batch.begin() + block1.size(), block2.size(), block2.begin());
            }

            // --- Move Semantics ---
            // To perform a move, adapt the source iterator with `std::make_move_iterator`.
            // The std::copy_n algorithm will then invoke the move-assignment operator.
            // Note: the source container (`sub_batch` here) must be mutable.
            //
            // auto move_iter = std::make_move_iterator(sub_batch.begin());
            // std::copy_n(move_iter, block1.size(), block1.begin());
            // if (!block2.empty()) {
            //     std::copy_n(move_iter + block1.size(), block2.size(), block2.begin());
            // }
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
        // Note: To move data, wrap the source iterator with `std::make_move_iterator`
        // as in the example above.
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

## 4. Range-Based API (`std::ranges`)

For maximum convenience and compatibility with modern C++ algorithms, both the `WriteScope` and `ReadScope` objects are fully compliant with the C++20 `std::ranges` library. This allows for direct, elegant iteration, completely abstracting away the two-block nature of the circular buffer.

### Writing with a Range-Based `for` Loop

This is a clear and idiomatic way to fill the reserved slots in a write transaction.

```cpp
void range_based_producer(LockFreeSpscQueue<int>& queue)
{
    // Ask to reserve space for 10 items.
    if (auto write_scope = queue.prepare_write(10)) {
        // The WriteScope object is directly iterable.
        // We can iterate over the empty slots and write to them.
        int i = 0;
        for (int& slot : write_scope) {
            slot = i++;
        }
    }
    // The write is automatically committed when `write_scope` is destroyed.
}
```

### Reading with a Range-Based `for` Loop

This is the simplest way to consume data from the queue. The custom iterator handles the jump between the two memory blocks transparently.

```cpp
void range_based_consumer(LockFreeSpscQueue<int>& queue)
{
    // Ask to read up to 16 items at a time.
    if (auto read_scope = queue.prepare_read(16))
    {
        // The ReadScope object is a C++20 range.
        // The for loop will seamlessly iterate over block1 and then block2.
        for (const int& item : read_scope)
        {
            std::cout << "Consumer: Got " << item << "\n";
        }
    }
    // The read is automatically committed when `read_scope` is destroyed.
}
```

### Using with `std::ranges` Algorithms

Because the `Scope` objects are proper ranges, you can use them with the powerful algorithms from the `<algorithm>` and `<numeric>` headers.

```cpp
#include <numeric>   // For std::accumulate
#include <algorithm> // For std::ranges::copy

void algorithm_example(LockFreeSpscQueue<int>& queue)
{
    // Use std::ranges::copy to fill a write scope from another container.
    if (auto write_scope = queue.prepare_write(10))
    {
        std::vector<int> source_data(write_scope.get_items_written(), 42); // Fill with 42s
        std::ranges::copy(source_data, write_scope.begin());
    }

    // Use std::accumulate to sum all the items in a read scope.
    if (auto read_scope = queue.prepare_read(10))
    {
        long long sum = std::accumulate(read_scope.begin(), read_scope.end(), 0LL);
        std::cout << "Sum of items in queue: " << sum << "\n";
    }

    // -- More Examples --
    std::vector<int> source_data(16, 42); // A vector with 16 items of value 42.

    size_t total_written = 0;
    while (total_written < source_data.size())
    {
        // Ask the queue for space for the remaining items.
        if (auto write_scope = queue.prepare_write(source_data.size() - total_written))
        {
            // The number of slots we were actually provided.
            const size_t can_write_count = write_scope.get_items_written();

            // --- Copy Semantics ---
            // Create a sub-span of our source data that is _exactly_ the size of the
            // space we were granted.
            std::span<const int> source_sub_batch(source_data.data() + total_written,
                                                  can_write_count);

            // Copy the sub-batch into the write_scope's range.
            std::ranges::copy(source_sub_batch, write_scope.begin());

            // --- Move Semantics ---
            // To move instead of copy, simply adapt the source range with `std::views::move`.
            // Note: the source container (`source_data` here) must be mutable.
            //
            // auto source_sub_range = std::span(source_data).subspan(total_written,
            //                                                        can_write_count);
            // std::ranges::copy(source_sub_range | std::views::move, write_scope.begin());

            total_written += can_write_count;
        }
        else
        {
            // The queue was full, wait a moment and try again.
            std::this_thread::yield();
        }
    }
}
```

