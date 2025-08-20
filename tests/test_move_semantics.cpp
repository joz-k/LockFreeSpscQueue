#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <random>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <ranges>

#include "LockFreeSpscQueue.h"

// Test Configuration

const size_t QUEUE_CAPACITY          = 256; // Must be a power of two
const size_t TOTAL_ITEMS_TO_TRANSFER = 4000000;
const uint64_t RANDOM_SEED           = 20240819;

// Helper `assert` which aborts also in the release builds
#define always_assert(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ \
                      << ": " #condition << std::endl; \
            std::exit(EXIT_FAILURE); \
        } \
    } while (false)


// The data structure for queue items
// Contains non-trivial types to properly test move semantics.
struct Item
{
    std::string s;
    std::vector<size_t> i;

    Item() = default;

    // A move-constructing constructor for try_emplace testing
    Item(std::string&& str, std::vector<size_t>&& vec)
        : s(std::move(str)), i(std::move(vec)) {}

    // Equality operator for verification
    bool operator==(const Item& other) const {
        return s == other.s && i == other.i;
    }
};

// A pseudo-random generator for producing a deterministic sequence of Items
class ItemGenerator
{
public:
    explicit ItemGenerator(uint64_t seed)
        : m_engine(seed)
        , m_char_dist('a', 'z')
        , m_num_dist(0, std::numeric_limits<size_t>::max())
    {}

    Item next() {
        Item item;
        item.s.resize(8);
        for (char& c : item.s) {
            c = static_cast<char>(m_char_dist(m_engine));
        }
        item.i.resize(8);
        for (size_t& num : item.i) {
            num = m_num_dist(m_engine);
        }
        return item;
    }

private:
    std::mt19937_64 m_engine;
    std::uniform_int_distribution<int> m_char_dist;
    std::uniform_int_distribution<size_t> m_num_dist;
};


// --- Test 1: WriteTransaction::try_push with prepare_read ---
// This tests the `try_start_write` API for a single-item move-in against the
// low-level move-out API.
void test_transaction_push_with_prepare_read()
{
    std::cout << "Running Test 1: WriteTransaction::try_push -> prepare_read...\n";

    std::vector<Item> buffer(QUEUE_CAPACITY);
    LockFreeSpscQueue<Item> queue(buffer);

    auto producer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t items_written = 0;
        while (items_written < TOTAL_ITEMS_TO_TRANSFER) {
            if (auto transaction = queue.try_start_write(1)) {
                Item item_to_move = generator.next();

                always_assert(transaction->try_push(std::move(item_to_move)));
                always_assert(item_to_move.s.empty());
                always_assert(item_to_move.i.empty());

                items_written++;
            } else {
                std::this_thread::yield();
            }
        }
    };

    auto consumer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t items_read = 0;

        while (items_read < TOTAL_ITEMS_TO_TRANSFER) {
            // 1. Ask for a batch of items. We may get 1 or more depending on producer speed.
            auto read_scope = queue.prepare_read(16); // Ask for up to 16

            if (read_scope.get_items_read() > 0) {
                // 2. Iterate over the items ACTUALLY received.
                for (Item& received_item : read_scope) {
                    // 3. Generate the expected item.
                    Item expected_item = generator.next();
                    always_assert(expected_item == received_item);

                    // Move out and verify the move was successful.
                    Item consumed_item = std::move(received_item);
                    always_assert(received_item.s.empty());
                }

                // 4. Increment our total count by the number of items we just processed.
                items_read += read_scope.get_items_read();
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::jthread producer(producer_task);
    std::jthread consumer(consumer_task);
}

// --- Test 2: High-level try_write with try_read ---
// Tests the convenience batch APIs using move iterators.
void test_try_write_with_try_read()
{
    std::cout << "Running Test 2: try_write -> try_read...\n";

    std::vector<Item> buffer(QUEUE_CAPACITY);
    LockFreeSpscQueue<Item> queue(buffer);
    const size_t BATCH_SIZE = 32;

    auto producer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t items_written = 0;
        while (items_written < TOTAL_ITEMS_TO_TRANSFER) {
            // 1. Generate a local batch of items to move from.
            std::vector<Item> local_batch;
            size_t items_to_generate = std::min(BATCH_SIZE,
                                                TOTAL_ITEMS_TO_TRANSFER - items_written);
            local_batch.reserve(items_to_generate);
            for (size_t i = 0; i < items_to_generate; ++i) {
                local_batch.push_back(generator.next());
            }

            // 2. Attempt to write the batch, moving from our local items.
            size_t batch_written = 0;
            while (batch_written < local_batch.size()) {
                std::span batch_to_write(local_batch.data() + batch_written,
                                         local_batch.size() - batch_written);

                // Use `try_write` lambda API
                size_t num_moved = queue.try_write(batch_to_write.size(),
                                                   [&](auto block1, auto block2)
                {
                    auto move_iter = std::make_move_iterator(batch_to_write.begin());
                    std::copy_n(move_iter, block1.size(), block1.begin());
                    if (!block2.empty()) {
                        std::copy_n(move_iter + block1.size(), block2.size(), block2.begin());
                    }
                });

                if (num_moved == 0) {
                    std::this_thread::yield();
                }
                batch_written += num_moved;
            }
            items_written += batch_written;
        }
    };

    auto consumer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);

        size_t items_read = 0;
        while (items_read < TOTAL_ITEMS_TO_TRANSFER) {
            // Use `try_read` lambda API
            size_t num_read = queue.try_read(BATCH_SIZE, [&](auto block1, auto block2) {
                for (const auto& received_item : block1) {
                    Item expected_item = generator.next();
                    always_assert(expected_item == received_item);
                }
                for (const auto& received_item : block2) {
                    Item expected_item = generator.next();
                    always_assert(expected_item == received_item);
                }
            });

            if (num_read == 0) {
                std::this_thread::yield();
            }
            items_read += num_read;
        }
    };

    std::jthread producer(producer_task);
    std::jthread consumer(consumer_task);
}

// --- Test 3: Low-level prepare_write with prepare_read + range-for ---
// Tests manual block management for moving data in, and range-based for-loop for moving data out.
void test_prepare_write_with_prepare_read_ranges()
{
    std::cout << "Running Test 3: prepare_write -> prepare_read (range-for)...\n";
    std::vector<Item> buffer(QUEUE_CAPACITY);
    LockFreeSpscQueue<Item> queue(buffer);
    const size_t BATCH_SIZE = 16;

    auto producer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t total_written = 0;
        while (total_written < TOTAL_ITEMS_TO_TRANSFER) {
            // 1. Prepare a local batch to move from.
            size_t items_to_generate = std::min(BATCH_SIZE,
                                                TOTAL_ITEMS_TO_TRANSFER - total_written);
            std::vector<Item> local_batch;
            local_batch.reserve(items_to_generate);
            for (size_t i = 0; i < items_to_generate; ++i) {
                local_batch.push_back(generator.next());
            }

            // 2. Move the batch using low-level `prepare_write` API.
            size_t batch_written = 0;
            while (batch_written < local_batch.size()) {
                auto write_scope = queue.prepare_write(local_batch.size() - batch_written);
                size_t can_write = write_scope.get_items_written();

                if (can_write > 0) {
                    auto block1 = write_scope.get_block1();
                    auto block2 = write_scope.get_block2();
                    std::span source_span(local_batch.data() + batch_written, can_write);

                    auto move_iter = std::make_move_iterator(source_span.begin());
                    std::copy_n(move_iter, block1.size(), block1.begin());
                    if (!block2.empty()) {
                        std::copy_n(move_iter + block1.size(), block2.size(), block2.begin());
                    }
                } else {
                    std::this_thread::yield();
                    continue;
                }
                batch_written += write_scope.get_items_written();
            }
            total_written += batch_written;
        }
    };

    auto consumer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t total_read = 0;

        while (total_read < TOTAL_ITEMS_TO_TRANSFER) {
            auto read_scope = queue.prepare_read(BATCH_SIZE);
            if (read_scope.get_items_read() > 0) {
                for (Item& received_item : read_scope) {
                    Item expected_item = generator.next();
                    always_assert(expected_item == received_item);
                    // Move out and verify source is now empty
                    Item consumed = std::move(received_item);
                    always_assert(received_item.s.empty());
                }
                total_read += read_scope.get_items_read();
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::jthread producer(producer_task);
    std::jthread consumer(consumer_task);
}

// --- Test 4: Full C++20 Ranges API ---
// Tests moving data in with std::ranges::copy and a move-transform view, and
// reading it back with a standard range-for loop.
void test_full_ranges_api()
{
    std::cout << "Running Test 4: C++20 Ranges API (move in/read out)...\n";
    std::vector<Item> buffer(QUEUE_CAPACITY);
    LockFreeSpscQueue<Item> queue(buffer);
    const size_t BATCH_SIZE = 64;

    auto producer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t total_written = 0;
        while (total_written < TOTAL_ITEMS_TO_TRANSFER) {
            size_t items_to_generate = std::min(BATCH_SIZE,
                                                TOTAL_ITEMS_TO_TRANSFER - total_written);
            std::vector<Item> local_batch;
            for (size_t i=0; i<items_to_generate; ++i) {
                local_batch.push_back(generator.next());
            }

            size_t batch_written = 0;
            while (batch_written < local_batch.size()) {
                auto write_scope = queue.prepare_write(local_batch.size() - batch_written);
                if (write_scope.get_items_written() > 0) {
                    size_t can_write = write_scope.get_items_written();
                    std::span source_span(local_batch.data() + batch_written, can_write);

                    auto to_move_view = std::views::transform([](auto&& item) -> decltype(auto) {
                        return std::move(item);
                    });
                    std::ranges::copy(source_span | to_move_view, write_scope.begin());

                    batch_written += can_write;
                } else {
                    std::this_thread::yield();
                }
            }
            total_written += batch_written;
        }
    };

    auto consumer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t total_read = 0;
        while (total_read < TOTAL_ITEMS_TO_TRANSFER) {
            auto read_scope = queue.prepare_read(BATCH_SIZE);
            if (read_scope.get_items_read() > 0) {
                for (const auto& received_item : read_scope) {
                    Item expected_item = generator.next();
                    always_assert(expected_item == received_item);
                }
                total_read += read_scope.get_items_read();
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::jthread producer(producer_task);
    std::jthread consumer(consumer_task);
}

// --- Test 5: try_emplace with C++23 std::views::as_rvalue ---
// Tests in-place construction on the producer side against the modern C++23
// range-based move-out API on the consumer side.
void test_emplace_with_ranges_as_rvalue()
{
    std::cout << "Running Test 5: try_emplace -> std::views::as_rvalue...\n";
    std::vector<Item> buffer(QUEUE_CAPACITY);
    LockFreeSpscQueue<Item> queue(buffer);
    const size_t BATCH_SIZE = 32;

    auto producer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t items_written = 0;
        while (items_written < TOTAL_ITEMS_TO_TRANSFER) {
            // Start a transaction for a batch.
            if (auto transaction = queue.try_start_write(BATCH_SIZE)) {
                size_t emplaced_in_tx = 0;
                // Keep emplacing until the transaction is full.
                while (emplaced_in_tx < transaction->capacity()) {
                    Item item_to_emplace = generator.next();
                    // This is the core of the emplace test.
                    // It moves the string and vector directly into the Item's constructor.
                    // In a real scenario, there would be no object created in advance.
                    if (!transaction->try_emplace(std::move(item_to_emplace.s),
                                                  std::move(item_to_emplace.i)))
                    {
                        break; // Transaction is full
                    }
                    emplaced_in_tx++;
                }
                items_written += emplaced_in_tx;
            } else {
                std::this_thread::yield();
            }
        }
    };

    auto consumer_task = [&]() {
        ItemGenerator generator(RANDOM_SEED);
        size_t items_read = 0;
        while (items_read < TOTAL_ITEMS_TO_TRANSFER) {
            auto read_scope = queue.prepare_read(BATCH_SIZE);
            if (read_scope.get_items_read() > 0) {
                std::vector<Item> consumed_batch;
                consumed_batch.reserve(read_scope.get_items_read());

                // The core of the C++23 move-out test:
                // `as_rvalue` turns the range of T& into a range of T&&.
                // `std::ranges::copy` then invokes the move constructor for each element.
                std::ranges::copy(read_scope | std::views::as_rvalue,
                                  std::back_inserter(consumed_batch));

                // After moving, verify that the items in the queue's buffer are now empty.
                for (const auto& item_in_queue : read_scope) {
                    always_assert(item_in_queue.s.empty());
                    always_assert(item_in_queue.i.empty());
                }

                // Verify the content of the items we moved into our local vector.
                for (const auto& consumed_item : consumed_batch) {
                    Item expected_item = generator.next();
                    always_assert(expected_item == consumed_item);
                }
                items_read += consumed_batch.size();
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::jthread producer(producer_task);
    std::jthread consumer(consumer_task);
}


int main()
{
    try {
        test_transaction_push_with_prepare_read();
        std::cout << "Test 1 PASSED.\n\n";

        test_try_write_with_try_read();
        std::cout << "Test 2 PASSED.\n\n";

        test_prepare_write_with_prepare_read_ranges();
        std::cout << "Test 3 PASSED.\n\n";

        test_full_ranges_api();
        std::cout << "Test 4 PASSED.\n\n";

        test_emplace_with_ranges_as_rvalue();
        std::cout << "Test 5 PASSED.\n\n";

        std::cout << "All move semantics tests PASSED.\n";

    } catch (const std::exception& e) {
        std::cerr << "A test failed with exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

