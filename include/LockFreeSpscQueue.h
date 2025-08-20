// MIT License
//
// Copyright (c) 2025 Jozef Kosoru
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <span>
#include <stdexcept>
#include <bit>
#include <new>
#include <optional>

/**
 * @class LockFreeSpscQueue
 * @brief A lock-free, single-producer, single-consumer (SPSC) queue that manages
 *        indices for a user-provided ring buffer.
 *
 * @details This class takes a non-owning view (`std::span`) of a user-provided buffer
 *          at construction. It safely manages read and write indices for that buffer.
 *          This design is inspired by `juce::AbstractFifo` and is suitable for
 *          high-performance real-time applications.
 *
 * Key properties for safe and correct operation:
 * 1.  **SPSC Only**: This queue is **only safe** with one producer and one consumer thread.
 * 2.  **Power of Two Capacity**: The capacity of the buffer **must** be a power of two.
 * 3.  **RAII Scopes**: `prepare_write` and `prepare_read` return scope objects that
 *     automatically commit the transaction upon destruction (RAII).
 *
 * @tparam T The type of the object being stored in the user-provided buffer.
 */
template <typename T>
class LockFreeSpscQueue
{
    // --- Compile-Time Contract Enforcement ---
    // This is a critical check. The entire purpose of this class is to be lock-free.
    // This static_assert guarantees that the atomic type we use for our indices
    // is actually lock-free on the target platform. If it's not, the compiler
    // would have to use locks to emulate atomicity, silently destroying our
    // performance guarantees. This assertion makes the build fail instead.
    static_assert(std::atomic<size_t>::is_always_lock_free,
                  "LockFreeSpscQueue requires std::atomic<size_t> to be lock-free.");

public:
    // --- Public Scope Objects for RAII ---

    /**
     * @brief An RAII scope object representing a prepared write operation.
     * @details Provides direct `std::span` access to the writable blocks in the
     *          underlying buffer. The transaction is committed when this object
     *          is destroyed. This object also satisfies the `std::ranges::range`
     *          concept, allowing for direct iteration over the writable slots.
     *          It is move-only.
     * @warning The user MUST write a number of items exactly equal to the value
     *          returned by `get_items_written()`. Failure to do so will result
     *          in the consumer reading uninitialized/garbage data.
     */
    struct WriteScope
    {
        // --- Custom Iterator for WriteScope ---
        class iterator
        {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type        = T;
            using difference_type   = std::ptrdiff_t;
            using pointer           = T*;
            using reference         = T&;

            iterator() = default;
            reference operator*() const { return *m_current_ptr; }
            pointer operator->() const { return m_current_ptr; }

            iterator& operator++() {
                ++m_current_ptr;
                if (m_in_block1 && m_current_ptr == m_block1_end_ptr) {
                    m_current_ptr = m_block2_begin_ptr;
                    m_in_block1 = false;
                }
                return *this;
            }
            iterator operator++(int) { iterator tmp = *this; ++(*this); return tmp; }
            bool operator==(const iterator& other) const {
                // For two iterators to be equal, they only need to point to the same element.
                // The other members define the boundaries of the range, which are guaranteed
                // to be the same if the iterators originate from the same WriteScope object,
                // a precondition for valid comparison.
                return m_current_ptr == other.m_current_ptr;
            }

        private:
            friend struct WriteScope;
            iterator(std::span<T> b1, std::span<T> b2, bool is_begin)
                : m_block1_end_ptr(b1.data() + b1.size())
                , m_block2_begin_ptr(b2.data())
            {
                if (is_begin) {
                    if (!b1.empty()) { // If block1 is not empty, start there.
                        m_current_ptr = b1.data();
                        m_in_block1   = true;
                    } else { // Otherwise, start at block2.
                        m_current_ptr = b2.data();
                        m_in_block1   = false;
                    }
                } else { // This is the end() sentinel iterator.
                    m_current_ptr = b2.data() + b2.size();
                    m_in_block1   = false;
                }
            }

            pointer m_current_ptr      = nullptr;
            pointer m_block1_end_ptr   = nullptr;
            pointer m_block2_begin_ptr = nullptr;
            bool m_in_block1 = false;
        };

        // --- Making WriteScope a C++20 Range ---
        [[nodiscard]] iterator begin() {
            return iterator(get_block1(), get_block2(), true);
        }
        [[nodiscard]] iterator end() {
            return iterator(get_block1(), get_block2(), false);
        }

        /** @brief Returns a span representing the first contiguous block to write to. */
        [[nodiscard]] std::span<T> get_block1() const
        {
            return m_owner_queue
                ? m_owner_queue->m_buffer.subspan(start_index1, block_size1)
                : std::span<T>{};
        }

        /** @brief Returns a span representing the second contiguous block to
         *         write to (for wrap-around). */
        [[nodiscard]] std::span<T> get_block2() const
        {
            return m_owner_queue && block_size2 > 0
                ? m_owner_queue->m_buffer.subspan(start_index2, block_size2)
                : std::span<T>{};
        }

        /** @brief Returns the total number of items that were successfully
         *         prepared for writing. */
        [[nodiscard]] size_t get_items_written() const { return block_size1 + block_size2; }

        ~WriteScope() noexcept
        {
            if (m_owner_queue != nullptr) {
                m_owner_queue->commit_write(get_items_written());
                m_owner_queue = nullptr;
            }
        }

        // This RAII object is move-only to ensure single ownership of a transaction.
        WriteScope(const WriteScope&) = delete;
        WriteScope& operator=(const WriteScope&) = delete;

        WriteScope(WriteScope&& other) noexcept
            : start_index1(other.start_index1), block_size1(other.block_size1)
            , start_index2(other.start_index2), block_size2(other.block_size2)
            , m_owner_queue(other.m_owner_queue)
        { other.m_owner_queue = nullptr; }

        // Move assignment is deleted. It is not possible to assign to the const members,
        // and it makes little semantic sense for this single-transaction RAII type.
        WriteScope& operator=(WriteScope&& other) = delete;

    private:
        friend class LockFreeSpscQueue;
        WriteScope(size_t s1, size_t b1, size_t s2, size_t b2, LockFreeSpscQueue* owner)
            : start_index1(s1), block_size1(b1)
            , start_index2(s2), block_size2(b2)
            , m_owner_queue(owner) {}

        // Private members used to construct the spans on demand.
        const size_t start_index1 = 0;
        const size_t block_size1  = 0;
        const size_t start_index2 = 0;
        const size_t block_size2  = 0;
        LockFreeSpscQueue* m_owner_queue = nullptr;
    };

    /**
     * @brief An RAII scope object representing a prepared read operation.
     * @details Provides direct `std::span` access to readable blocks in the
     *          underlying buffer. The transaction is committed when this
     *          object is destroyed. It also satisfies the `std::ranges::range`
     *          concept, allowing direct iteration. If the scope is non-const,
     *          it allows "moving" elements out of the queue. If it is const, it
     *          provides read-only access. It is move-only.
     * @warning The user MUST treat all data within the returned spans as read.
     *          The full `get_items_read()` amount will be committed, advancing
     *          the read pointer and making the space available for future writes.
     */
    struct ReadScope
    {
        // --- Custom Iterators (const and non-const) ---
        template<bool IsConst>
        class any_iterator
        {
        public:
            using value_type      = std::conditional_t<IsConst, const T, T>;
            using difference_type = std::ptrdiff_t;
            using pointer         = value_type*;
            using reference       = value_type&;
            using Span            = std::span<value_type>;

            any_iterator() = default;
            reference operator*() const { return *m_current_ptr; }
            pointer operator->() const { return m_current_ptr; }

            any_iterator& operator++() {
                ++m_current_ptr;
                if (m_in_block1 && m_current_ptr == m_block1_end_ptr) {
                    m_current_ptr = m_block2_begin_ptr;
                    m_in_block1   = false;
                }
                return *this;
            }
            any_iterator operator++(int) { any_iterator tmp = *this; ++(*this); return tmp; }

            // Allow conversion from mutable iterator to const_iterator
            template<bool OtherIsConst, typename = std::enable_if_t<IsConst && !OtherIsConst>>
            any_iterator(const any_iterator<OtherIsConst>& other)
                : m_current_ptr(other.m_current_ptr)
                , m_block1_end_ptr(other.m_block1_end_ptr)
                , m_block2_begin_ptr(other.m_block2_begin_ptr)
                , m_in_block1(other.m_in_block1) {}

            bool operator==(const any_iterator& other) const {
                // Only the current position needs to be compared.
                return m_current_ptr == other.m_current_ptr;
            }

        private:
            friend struct ReadScope;
            template<bool> friend class any_iterator; // Allow conversion access

            any_iterator(Span b1, Span b2, bool is_begin)
                : m_block1_end_ptr(b1.data() + b1.size())
                , m_block2_begin_ptr(b2.data())
            {
                if (is_begin) {
                    if (!b1.empty()) { // If block 1 is not empty, start there.
                        m_current_ptr = b1.data();
                        m_in_block1   = true;
                    } else { // Otherwise, start at block 2.
                        m_current_ptr = b2.data();
                        m_in_block1   = false;
                    }
                } else { // This is the end() sentinel iterator.
                    m_current_ptr = b2.data() + b2.size();
                    m_in_block1   = false;
                }
            }

            pointer m_current_ptr      = nullptr;
            pointer m_block1_end_ptr   = nullptr;
            pointer m_block2_begin_ptr = nullptr;
            bool m_in_block1           = false;

            template<bool> friend class any_iterator; // Allow conversion access
        };

        /** @brief A mutable iterator for the ReadScope, enabling moves. */
        using iterator       = any_iterator<false>;
        using const_iterator = any_iterator<true>;

        // --- Making ReadScope a C++20 Range (with const and non-const overloads) ---
        [[nodiscard]] iterator begin() {
            return iterator(get_block1(), get_block2(), true);
        }
        [[nodiscard]] iterator end() {
            return iterator(get_block1(), get_block2(), false);
        }
        [[nodiscard]] const_iterator begin() const {
            return const_iterator(get_block1(), get_block2(), true);
        }
        [[nodiscard]] const_iterator end() const {
            return const_iterator(get_block1(), get_block2(), false);
        }

        // --- Block Accessors with const and non-const Overloads ---
        /** @brief Returns a mutable span to the first contiguous block. Enables moving. */
        [[nodiscard]] std::span<T> get_block1() {
            return m_owner_queue
                ? m_owner_queue->m_buffer.subspan(start_index1, block_size1)
                : std::span<T>{};
        }
        /** @brief Returns a read-only span to the first contiguous block. */
        [[nodiscard]] std::span<const T> get_block1() const {
            return m_owner_queue
                ? m_owner_queue->m_buffer.subspan(start_index1, block_size1)
                : std::span<const T>{};
        }

        /** @brief Returns a mutable span to the second contiguous block. Enables moving. */
        [[nodiscard]] std::span<T> get_block2() {
            return m_owner_queue && block_size2 > 0
                ? m_owner_queue->m_buffer.subspan(start_index2, block_size2)
                : std::span<T>{};
        }
        /** @brief Returns a read-only span to the second contiguous block. */
        [[nodiscard]] std::span<const T> get_block2() const {
            return m_owner_queue && block_size2 > 0
                ? m_owner_queue->m_buffer.subspan(start_index2, block_size2)
                : std::span<const T>{};
        }

        /** @brief Returns the total number of items that were successfully
         *         prepared for reading. */
        [[nodiscard]] size_t get_items_read() const { return block_size1 + block_size2; }

        ~ReadScope() noexcept
        {
            if (m_owner_queue != nullptr) {
                m_owner_queue->commit_read(get_items_read());
                m_owner_queue = nullptr;
            }
        }

        // This RAII object is move-only to ensure single ownership of a transaction.
        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;

        ReadScope(ReadScope&& other) noexcept
            : start_index1(other.start_index1), block_size1(other.block_size1)
            , start_index2(other.start_index2), block_size2(other.block_size2)
            , m_owner_queue(other.m_owner_queue)
        { other.m_owner_queue = nullptr; }

        // Move assignment is deleted. It is not possible to assign to the const members,
        // and it makes little semantic sense for this single-transaction RAII type.
        ReadScope& operator=(ReadScope&& other) = delete;

    private:
        friend class LockFreeSpscQueue;
        ReadScope(size_t s1, size_t b1, size_t s2, size_t b2, LockFreeSpscQueue* owner)
            : start_index1(s1), block_size1(b1)
            , start_index2(s2), block_size2(b2)
            , m_owner_queue(owner) {}
        const size_t start_index1 = 0;
        const size_t block_size1  = 0;
        const size_t start_index2 = 0;
        const size_t block_size2  = 0;
        LockFreeSpscQueue* m_owner_queue = nullptr;
    };

    // --- The Transaction Bulk-Write API RAII Object ---

    /**
     * @brief An RAII object representing a bulk write transaction.
     * @details This token reserves a block of space and allows for multiple,
     *          ultra-fast, non-atomic `try_push` operations within that space.
     *          The transaction is committed for the number of items actually
     *          pushed when this object is destroyed. It is move-only.
     */
    class WriteTransaction
    {
    public:
        /**
         * @brief Tries to push a single item into the reserved transaction space.
         * @details This is an extremely fast, non-atomic operation.
         * @return True if the item was pushed, false if the transaction's
         *         reserved space is full.
         */
        bool try_push(const T& item)
        {
            if (m_items_pushed_count >= m_total_reserved_size) {
                return false;
            }

            // Construct a new object in its place by copying.
            *get_next_slot() = item;

            m_items_pushed_count++;
            return true;
        }

        // Overload for movable types
        bool try_push(T&& item)
        {
            if (m_items_pushed_count >= m_total_reserved_size) {
                return false;
            }

            // Construct a new object in its place by moving.
            *get_next_slot() = std::move(item);

            m_items_pushed_count++;
            return true;
        }

        /**
         * @brief Constructs an item in-place directly in the queue's buffer.
         * @details This is the most efficient way to add a complex object to the queue,
         *          as it avoids creating any temporary objects or performing move operations.
         *          It uses `std::construct_at` for maximum safety and correctness.
         * @tparam Args The types of arguments to forward to the object's constructor.
         * @return True if the item was emplaced, false if the transaction's
         *         reserved space is full.
         */
        template<typename... Args>
        bool try_emplace(Args&&... args)
            noexcept(std::is_nothrow_constructible_v<T, Args...>)
        {
            if (m_items_pushed_count >= m_total_reserved_size) {
                return false;
            }

            T* slot_ptr = get_next_slot();

            // First, destroy the (potentially moved-from) object in the slot.
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::destroy_at(slot_ptr);
            }

            // Construct the object directly in the queue's memory buffer.
            std::construct_at(slot_ptr, std::forward<Args>(args)...);

            m_items_pushed_count++;
            return true;
        }

        /** @brief Returns the total number of items this transaction can hold. */
        [[nodiscard]] size_t capacity() const { return m_total_reserved_size; }
        /** @brief Returns how many items have been pushed so far. */
        [[nodiscard]] size_t items_pushed() const { return m_items_pushed_count; }

        ~WriteTransaction()
        {
            if (m_owner_queue != nullptr) {
                m_owner_queue->commit_write(m_items_pushed_count);
                m_owner_queue = nullptr;
            }
        }

        WriteTransaction(const WriteTransaction&) = delete;
        WriteTransaction& operator=(const WriteTransaction&) = delete;
        WriteTransaction(WriteTransaction&& other) noexcept
            : m_owner_queue(other.m_owner_queue)
            , m_block1(other.m_block1), m_block2(other.m_block2)
            , m_total_reserved_size(other.m_total_reserved_size)
            , m_items_pushed_count(other.m_items_pushed_count)
        {
            other.m_owner_queue = nullptr;
            other.m_items_pushed_count = 0;
        }
        WriteTransaction& operator=(WriteTransaction&&) = delete;

    private:
        friend class LockFreeSpscQueue;
        WriteTransaction(LockFreeSpscQueue* owner, std::span<T> b1, std::span<T> b2)
            : m_owner_queue(owner)
            , m_block1(b1), m_block2(b2)
            , m_total_reserved_size(b1.size() + b2.size()) {}

        T* get_next_slot() {
            return (m_items_pushed_count < m_block1.size())
                ? &m_block1[m_items_pushed_count]
                : &m_block2[m_items_pushed_count - m_block1.size()];
        }

        LockFreeSpscQueue* m_owner_queue = nullptr;
        std::span<T> m_block1;
        std::span<T> m_block2;
        size_t m_total_reserved_size = 0;
        size_t m_items_pushed_count  = 0;
    };

    // --- Public API ---

    /**
     * @brief Constructs the queue manager.
     * @param buffer A std::span viewing the memory buffer this queue will manage.
     *               The size of this buffer MUST be a power of two.
     * @warning The user is responsible for ensuring that the lifetime of the
     *          provided buffer exceeds the lifetime of this queue object.
     *          The user is also reponsible for the destruction of any elements
     *          remaining in the buffer when it is no longer in use.
     */
    explicit LockFreeSpscQueue(std::span<T> buffer)
        : m_buffer(buffer), m_capacity(buffer.size()), m_capacity_mask(m_capacity - 1)
    {
        if (m_capacity == 0) {
            throw std::invalid_argument("Buffer capacity cannot be zero.");
        }

        if (!std::has_single_bit(m_capacity)) {
            throw std::invalid_argument("Buffer capacity must be a power of two.");
        }
    }

    /**
     * @brief Prepares a write operation for a specified number of items.
     * @details This should only be called by the single producer thread. The number
     *          of items for which space is reserved is returned via the `get_items_written()`
     *          method on the returned `WriteScope` object.
     * @note    If the writer is writing data at a rate much faster than the
     *          reader is able to consume, the `prepare_write()` method will inevitably
     *          return an empty `WriteScope` without any block containing a
     *          space for the writer. In such a case, the writer thread needs to
     *          wait for the reader thread to free up some space in the ring
     *          buffer.
     * @param num_items_to_write The maximum number of items you wish to write.
     * @return A `WriteScope` object detailing where to copy the data. The number of
     *         items that are actually prepared might be less than what was
     *         requested, or even zero if the queue is full.
     */
    [[nodiscard]] WriteScope prepare_write(size_t num_items_to_write)
    {
        auto [start_index, block_size1, start_index2, block_size2]
                                                    = get_write_reservation(num_items_to_write);
        if (block_size1 + block_size2 == 0) {
            return { 0, 0, 0, 0, nullptr };
        }

        return { start_index, block_size1, start_index2, block_size2, this };
    }

    /**
     * @brief Prepares a read operation for a specified number of items.
     * @details This should only be called by the single consumer thread. The number
     *          of items available to be read is returned via the `get_items_read()`
     *          method on the returned `ReadScope` object.
     * @param num_items_to_read The maximum number of items you wish to read.
     * @return A `ReadScope` object detailing where to copy data from. The number of
     *         items prepared for reading may be less than requested if the queue
     *         has fewer items available.
     */
    [[nodiscard]] ReadScope prepare_read(size_t num_items_to_read)
    {
        // "Fast path" calculation using the consumer's local cache of the write
        // pointer and the `read_pos`. Relaxed load for the `read_pos` is
        // safe for the read pointer as this is the only thread that modifies it.
        const size_t current_read_pos
                        = m_consumer_data.read_pos.load(std::memory_order_relaxed);
        size_t available_items
                        = m_consumer_data.cached_write_pos - current_read_pos;

        if (available_items < num_items_to_read) {
            // "Slow path": our cache is out of date.
            // Perform an expensive acquire load to get the true position from the producer.
            m_consumer_data.cached_write_pos
                            = m_producer_data.write_pos.load(std::memory_order_acquire);
            // Recalculate available items with the updated value.
            available_items = m_consumer_data.cached_write_pos - current_read_pos;
        }

        const size_t items_to_read = std::min(num_items_to_read, available_items);
        if (items_to_read == 0) { return {0, 0, 0, 0, nullptr}; }

        const size_t start_index  = current_read_pos & m_capacity_mask;
        const size_t items_to_end = m_capacity - start_index;
        const size_t block_size1  = std::min(items_to_read, items_to_end);
        const size_t block_size2  = items_to_read - block_size1;
        return {start_index, block_size1, 0, block_size2, this};
    }

    /**
     * @brief Tries to start a bulk write transaction.
     * @param num_items The desired number of items to reserve space for.
     * @return An std::optional containing a WriteTransaction if at least space
     *         for a single item was successfully reserved, otherwise std::nullopt.
     */
    [[nodiscard]] std::optional<WriteTransaction> try_start_write(size_t num_items)
    {
        auto [start_index, block_size1, start_index2, block_size2]
                                                            = get_write_reservation(num_items);

        const size_t items_reserved = block_size1 + block_size2;
        if (items_reserved == 0) {
            return std::nullopt;
        }

        auto block1 = m_buffer.subspan(start_index, block_size1);
        auto block2 = (block_size2 > 0)
                    ? m_buffer.subspan(start_index2, block_size2)
                    : std::span<T>{};

        return WriteTransaction(this, block1, block2);
    }

    /** @brief Returns the total capacity of the queue (the size of the buffer). */
    [[nodiscard]] size_t get_capacity() const noexcept { return m_capacity; }

    /**
     * @brief Returns the number of items currently available to be read.
     * @details This value should be treated as a hint, as the state can change
     *          immediately after this call due to the producer thread's activity.
     */
    [[nodiscard]] size_t get_num_items_ready() const noexcept
    {
        return m_producer_data.write_pos.load(std::memory_order_relaxed)
                                - m_consumer_data.read_pos.load(std::memory_order_relaxed);
    }

    /**
     * @brief Returns the number of items that can currently be written to the queue.
     * @details This value should be treated as a hint, as the state can change
     *          immediately after this call due to the consumer thread's activity.
     */
    [[nodiscard]] size_t get_num_free() const noexcept
    {
        return m_capacity - get_num_items_ready();
    }

    // --- High-Level Convenience API ---

    /**
     * @brief Tries to write a batch of items using a user-provided writer function.
     * @details This is a convenience wrapper around `prepare_write`. It handles the
     *          scope management and provides the writer function with spans to the
     *          writable blocks.
     * @tparam Func A callable type that takes two `std::span<T>` arguments.
     * @param num_items The maximum number of items to write.
     * @param writer The function to be called if space is available. It will be
     *               invoked with `writer(block1, block2)`.
     * @return The number of items successfully written (which may be less than
     *         `num_items`), or 0 if the queue was full.
     */
    template<typename Func>
    [[nodiscard]] size_t try_write(size_t num_items, Func&& writer)
        noexcept(std::is_nothrow_invocable_v<Func, std::span<T>, std::span<T>>)
    {
        auto scope = prepare_write(num_items);
        const size_t items_written = scope.get_items_written();
        if (items_written > 0) {
            // The user's writer function is responsible for filling the blocks.
            std::invoke(std::forward<Func>(writer), scope.get_block1(), scope.get_block2());
        }
        // The write is automatically committed when `scope` is destroyed here.
        return items_written;
    }

    /**
     * @brief Tries to read a batch of items using a user-provided reader function.
     * @details This is a convenience wrapper around `prepare_read`. It handles the
     *          scope management and provides the reader function with spans to the
     *          readable blocks.
     * @tparam Func A callable type that takes two `std::span<const T>` arguments.
     * @param num_items The maximum number of items to read.
     * @param reader The function to be called if items are available. It will be
     *               invoked with `reader(block1, block2)`.
     * @return The number of items successfully read (which may be less than
     *         `num_items`), or 0 if the queue was empty.
     */
    template<typename Func>
    [[nodiscard]] size_t try_read(size_t num_items, Func&& reader)
        noexcept(std::is_nothrow_invocable_v<Func, std::span<const T>, std::span<const T>>)
    {
        auto scope = prepare_read(num_items);
        const size_t items_read = scope.get_items_read();
        if (items_read > 0) {
            // The user's reader function is responsible for processing the blocks.
            std::invoke(std::forward<Func>(reader), scope.get_block1(), scope.get_block2());
        }
        // The read is automatically committed when `scope` is destroyed here.
        return items_read;
    }

private:
    friend struct WriteScope;
    friend struct ReadScope;

    /**
     * @brief Performs the core logic to calculate a reservation for a write operation.
     * @details This is a private helper that centralizes the write reservation logic,
     *          which is shared by `prepare_write` and `try_start_write`. It implements
     *          the "fast path/slow path" optimization by first checking against the
     *          producer's cached `read_pos` and only performing an expensive `acquire`
     *          load on the consumer's true `read_pos` when necessary.
     * @note This function is responsible only for the calculation of a reservation;
     *       it does not perform the final "commit" that makes the space available. While
     *       it may update the producer's internal `cached_read_pos` as a performance
     *       optimization, it never modifies the queue's main `write_pos` index.
     *       The actual commit (advancing `write_pos`) is the exclusive responsibility
     *       of the RAII scope objects (`WriteScope`, `WriteTransaction`).
     * @return A tuple containing {start_index1, block_size1, start_index2, block_size2}.
     *         If no space is available, all values in the tuple will be zero.
     */
    [[nodiscard]] std::tuple<size_t, size_t, size_t, size_t>
                                        get_write_reservation(size_t num_items_to_write) noexcept
    {
        // "Fast path" calculation using the producer's local cache of the read
        // pointer (which involves no cross-core communication) and `write_pos`.
        // Relaxed load is safe for the `write_pos` index as this is the only
        // thread that modifies it.
        const size_t current_write_pos
                            = m_producer_data.write_pos.load(std::memory_order_relaxed);
        size_t available_space
                            = m_capacity - (current_write_pos - m_producer_data.cached_read_pos);

        // Note: The subtraction `current_write_pos - cached_read_pos`
        // calculates the number of items currently in the queue, even when the
        // 64-bit indices wrap around, due to the defined behavior of unsigned
        // integer arithmetic.

        if (available_space < num_items_to_write) {
            // "Slow path": our cache is out of date.
            // Perform an expensive acquire load to get the true position from the consumer.
            m_producer_data.cached_read_pos
                            = m_consumer_data.read_pos.load(std::memory_order_acquire);
            available_space = m_capacity - (current_write_pos - m_producer_data.cached_read_pos);
        }

        const size_t items_to_reserve = std::min(num_items_to_write, available_space);

        if (items_to_reserve == 0) {
            return { 0, 0, 0, 0 };
        }

        const size_t start_index  = current_write_pos & m_capacity_mask;
        const size_t space_to_end = m_capacity - start_index;
        const size_t block_size1  = std::min(items_to_reserve, space_to_end);
        const size_t block_size2  = items_to_reserve - block_size1;
        return { start_index, block_size1, 0, block_size2 };
    }

    void commit_write(size_t num_items_written) noexcept
    {
        // This function uses a load-then-store sequence, which is a deliberate
        // performance optimization. It is only safe because this is a single-producer
        // queue, meaning this is the ONLY thread that will ever write to `write_pos`.
        //
        // The alternative, more general-purpose atomic operation would be:
        //   write_pos.fetch_add(num_items_written, std::memory_order_release);
        //
        // However, `fetch_add` is a Read-Modify-Write (RMW) operation, which is
        // significantly more expensive than a simple store on most architectures.
        // We avoid the RMW operation by leveraging the SPSC guarantee.
        m_producer_data.write_pos.store(
                m_producer_data.write_pos.load(std::memory_order_relaxed) + num_items_written,
                std::memory_order_release);
    }

    void commit_read(size_t num_items_read) noexcept
    {
        // Similar to commit_write, this uses a load-then-store sequence as a
        // performance optimization, which is safe because of the single-consumer
        // guarantee for `read_pos`.
        //
        // The more general (and more expensive) RMW alternative would be:
        //   read_pos.fetch_add(num_items_read, std::memory_order_release);
        //
        m_consumer_data.read_pos.store(
                m_consumer_data.read_pos.load(std::memory_order_relaxed) + num_items_read,
                std::memory_order_release);
    }

    std::span<T> m_buffer;

    // This block handles a GCC-specific warning about ABI stability.
#if defined(__GNUC__) && !defined(__clang__)
    // GCC 12+ warns that the value of hardware_destructive_interference_size
    // can change with compiler flags, affecting ABI. As a header-only library,
    // we assume consistent flags for a given build. This pragma silences the
    // warning locally, allowing us to use the semantically correct standard constant.
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Winterference-size"
#endif
    // Define a stable constant for the cache line size.
    // This block provides a portable way to get the cache line size.
    // It uses the C++17 standard constant if available, otherwise falls back
    // to a sensible default. This handles toolchains (like the one on GitHub's
    // older macOS runners) where the standard library might not be fully C++17-compliant.
#if defined(__cpp_lib_hardware_interference_size) && !defined(__clang__)
    // This is the ideal path: the compiler and standard library are fully C++17-compliant.
    // We exclude Apple Clang because its libc++ can define the feature-test macro
    // without actually providing the constant, leading to a compilation error.
    static constexpr size_t CacheLineSize = std::hardware_destructive_interference_size;
#else
    // This is the fallback path for older compilers or for Apple Clang, where we
    // cannot trust the feature-test macro. 64 bytes is a safe and widely-used
    // default for modern hardware (x86-64, ARM).
    static constexpr size_t CacheLineSize = 64;
#endif
#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic pop
#endif

    // A proper cache line alignment prevents "false sharing". On modern CPUs,
    // memory is moved in cache lines. E.g. if `write_pos` (used by the producer) and
    // `read_pos` (used by the consumer) were to share a cache line, modifications
    // by one thread would invalidate the other thread's cache, causing significant
    // performance degradation.

    // Producer-only data. Grouped to prevent false sharing with consumer data.
    alignas(CacheLineSize) struct ProducerData {
        std::atomic<size_t> write_pos = {0};
        size_t        cached_read_pos = {0};
    } m_producer_data;

    // Consumer-only data. Grouped and aligned.
    alignas(CacheLineSize) struct ConsumerData {
        std::atomic<size_t> read_pos = {0};
        size_t      cached_write_pos = {0};
    } m_consumer_data;

    /// The total number of items the buffer can hold.
    const size_t m_capacity;
    /// A bitmask used for fast modulo operations (index & m_capacity_mask).
    const size_t m_capacity_mask;
};

