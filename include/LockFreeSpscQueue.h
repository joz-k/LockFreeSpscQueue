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

/**
 * @class LockFreeSpscQueue
 * @brief A lock-free, single-producer, single-consumer (SPSC) queue that manages
 *        indices for a user-provided buffer.
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
     *          is destroyed. It is move-only.
     * @warning The user MUST write a number of items exactly equal to the value
     *          returned by `get_items_written()`. Failure to do so will result
     *          in the consumer reading uninitialized/garbage data.
     */
    struct WriteScope
    {
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
            if (m_owner_queue != nullptr) { m_owner_queue->commit_write(get_items_written()); }
        }

        // This RAII object is move-only to ensure single ownership of a transaction.
        WriteScope(const WriteScope&) = delete;
        WriteScope& operator=(const WriteScope&) = delete;

        WriteScope(WriteScope&& other) noexcept
            : start_index1(other.start_index1), block_size1(other.block_size1),
              start_index2(other.start_index2), block_size2(other.block_size2),
              m_owner_queue(other.m_owner_queue)
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
     * @details Provides direct `std::span` access to the readable blocks in the
     *          underlying buffer. The transaction is committed when this object
     *          is destroyed. It is move-only.
     * @warning The user MUST treat all data within the returned spans as read.
     *          The full `get_items_read()` amount will be committed, advancing
     *          the read pointer and making the space available for future writes.
     */
    struct ReadScope
    {
        /** @brief Returns a span representing the first contiguous block to read from. */
        [[nodiscard]] std::span<const T> get_block1() const
        {
            return m_owner_queue
                ? m_owner_queue->m_buffer.subspan(start_index1, block_size1)
                : std::span<const T>{};
        }

        /** @brief Returns a span representing the second contiguous block to
         *         read from (for wrap-around). */
        [[nodiscard]] std::span<const T> get_block2() const
        {
            return m_owner_queue && block_size2 > 0
                ? m_owner_queue->m_buffer.subspan(start_index2, block_size2)
                : std::span<const T>{};
        }

        /** @brief Returns the total number of items that were successfully
         *         prepared for reading. */
        [[nodiscard]] size_t get_items_read() const { return block_size1 + block_size2; }

        ~ReadScope() noexcept
        {
            if (m_owner_queue != nullptr) { m_owner_queue->commit_read(get_items_read()); }
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

    // --- Public API ---

    /**
     * @brief Constructs the queue manager.
     * @param buffer A std::span viewing the memory buffer this queue will manage.
     *               The size of this buffer MUST be a power of two.
     * @warning The user is responsible for ensuring that the lifetime of the
     *          provided buffer exceeds the lifetime of this queue object.
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
     * @param num_items_to_write The maximum number of items you wish to write.
     * @return A `WriteScope` object detailing where to copy the data. The number of
     *         items actually prepared may be less than requested if the queue is full.
     */
    [[nodiscard]] WriteScope prepare_write(size_t num_items_to_write)
    {
        // Acquire load synchronizes-with the release store in commit_read, ensuring
        // we see the latest free space created by the consumer.
        const size_t current_read_pos  = m_read_pos.load(std::memory_order_acquire);
        // Relaxed load is safe for the write pointer as this is the only thread that modifies it.
        const size_t current_write_pos = m_write_pos.load(std::memory_order_relaxed);

        // This subtraction calculates the number of items currently in the queue,
        // even when the 64-bit indices wrap around, due to the defined behavior of
        // unsigned integer arithmetic.
        const size_t num_items_in_queue = current_write_pos - current_read_pos;
        const size_t available_space    = m_capacity - num_items_in_queue;
        const size_t items_to_write     = std::min(num_items_to_write, available_space);

        if (items_to_write == 0) { return {0, 0, 0, 0, nullptr}; }

        const size_t start_index  = current_write_pos & m_capacity_mask;
        const size_t space_to_end = m_capacity - start_index;
        const size_t block_size1  = std::min(items_to_write, space_to_end);
        const size_t block_size2  = items_to_write - block_size1;
        return {start_index, block_size1, 0, block_size2, this};
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
        // Acquire load synchronizes-with the release store in commit_write, ensuring
        // that any data writes made by the producer are visible before we read them.
        const size_t current_write_pos = m_write_pos.load(std::memory_order_acquire);
        // Relaxed load is safe for the read pointer as this is the only thread that modifies it.
        const size_t current_read_pos  = m_read_pos.load(std::memory_order_relaxed);

        // This subtraction calculates the number of items available to be read,
        // even across size_t wrap-around, due to unsigned integer properties.
        const size_t available_items = current_write_pos - current_read_pos;
        const size_t items_to_read   = std::min(num_items_to_read, available_items);

        if (items_to_read == 0) { return {0, 0, 0, 0, nullptr}; }

        const size_t start_index  = current_read_pos & m_capacity_mask;
        const size_t items_to_end = m_capacity - start_index;
        const size_t block_size1  = std::min(items_to_read, items_to_end);
        const size_t block_size2  = items_to_read - block_size1;
        return {start_index, block_size1, 0, block_size2, this};
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
        return m_write_pos.load(std::memory_order_relaxed)
                    - m_read_pos.load(std::memory_order_relaxed);
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

    void commit_write(size_t num_items_written) noexcept
    {
        // This function uses a load-then-store sequence, which is a deliberate
        // performance optimization. It is only safe because this is a single-producer
        // queue, meaning this is the ONLY thread that will ever write to `m_write_pos`.
        //
        // The alternative, more general-purpose atomic operation would be:
        //   m_write_pos.fetch_add(num_items_written, std::memory_order_release);
        //
        // However, `fetch_add` is a Read-Modify-Write (RMW) operation, which is
        // significantly more expensive than a simple store on most architectures.
        // We avoid the RMW operation by leveraging the SPSC guarantee.
        m_write_pos.store(m_write_pos.load(std::memory_order_relaxed) + num_items_written,
                          std::memory_order_release);
    }

    void commit_read(size_t num_items_read) noexcept
    {
        // Similar to commit_write, this uses a load-then-store sequence as a
        // performance optimization, which is safe because of the single-consumer
        // guarantee for `m_read_pos`.
        //
        // The more general (and more expensive) RMW alternative would be:
        //   m_read_pos.fetch_add(num_items_read, std::memory_order_release);
        //
        m_read_pos.store(m_read_pos.load(std::memory_order_relaxed) + num_items_read,
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
//#if defined(__cpp_lib_hardware_interference_size) && !defined(__apple_build_version__)
    // This is the ideal path: the compiler and standard library are fully C++17-compliant.
    // We exclude Apple Clang because its libc++ can define the feature-test macro
    // without actually providing the constant, leading to a compilation error.
//    static constexpr size_t CacheLineSize = std::hardware_destructive_interference_size;
//#else
    // This is the fallback path for older compilers or for Apple Clang, where we
    // cannot trust the feature-test macro. 64 bytes is a safe and widely-used
    // default for modern hardware (x86-64, ARM).
    static constexpr size_t CacheLineSize = 64;
//#endif
#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic pop
#endif

    // Ensure cache line alignment to prevent "false sharing". On modern CPUs,
    // memory is moved in cache lines. If m_write_pos (used by the producer) and
    // m_read_pos (used by the consumer) were to share a cache line, modifications
    // by one thread would invalidate the other thread's cache, causing significant
    // performance degradation. std::hardware_destructive_interference_size (C++17)
    // provides the platform's recommended alignment size to avoid this.
    alignas(CacheLineSize) std::atomic<size_t> m_write_pos = {0};
    alignas(CacheLineSize) std::atomic<size_t> m_read_pos  = {0};

    /// The total number of items the buffer can hold.
    const size_t m_capacity;
    /// A bitmask used for fast modulo operations (index & m_capacity_mask).
    const size_t m_capacity_mask;
};

