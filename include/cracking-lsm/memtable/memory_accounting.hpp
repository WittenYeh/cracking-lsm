// Copyright 2026 Weitang Ye
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <type_traits>

#include <cracking-lsm/utils/error.hpp>

namespace cracking_lsm {

/**
 * @brief Shared live-allocation byte count for one single-threaded Memtable.
 *
 * The owner must keep this object alive at a stable address until all allocators and allocations
 * using it are gone. Copying or moving the accounting object would break that association.
 */
class MemoryAccounting {
public:
    MemoryAccounting() noexcept = default;
    MemoryAccounting(const MemoryAccounting&) = delete;
    auto operator=(const MemoryAccounting&) -> MemoryAccounting& = delete;
    MemoryAccounting(MemoryAccounting&&) = delete;
    auto operator=(MemoryAccounting&&) -> MemoryAccounting& = delete;

    /** @brief Returns successful allocation request bytes that have not yet been released. */
    [[nodiscard]] auto allocated_bytes() const noexcept -> std::size_t {
        return allocated_bytes_;
    }

private:
    template <typename T>
    friend class CountingAllocator;

    std::size_t allocated_bytes_ = 0;
};

/**
 * @brief Standard allocation with shared byte accounting across copies and node rebinds.
 *
 * Counts n * sizeof(T), including padding in rebound node allocation types. Hidden system allocator
 * overhead is excluded. The allocator borrows its accounting object and never allocates an arena.
 * Allocation failure and accounting overflow/underflow report context to stderr, then terminate.
 * No exception leaves the allocator boundary.
 * Capacity thresholds belong to Memtable insertion and do not restrict allocations here.
 */
template <typename T>
class CountingAllocator {
public:
    // Required by std::allocator_traits.
    using value_type = T;
    using size_t = std::size_t;
    using difference_t = std::ptrdiff_t;

    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    template <typename U>
    struct rebind {
        using other = CountingAllocator<U>;
    };

    /** @brief Requires an explicit accounting domain; no unbound default allocator is provided. */
    explicit CountingAllocator(MemoryAccounting& accounting) noexcept : accounting_(&accounting) {}

    CountingAllocator(const CountingAllocator&) noexcept = default;
    auto operator=(const CountingAllocator&) noexcept -> CountingAllocator& = default;
    CountingAllocator(CountingAllocator&&) noexcept = default;
    auto operator=(CountingAllocator&&) noexcept -> CountingAllocator& = default;

    /** @brief Rebinding preserves the original accounting domain without allocating or resetting it. */
    template <typename U>
    CountingAllocator(const CountingAllocator<U>& other) noexcept : accounting_(other.accounting_) {}

    /** @brief Allocates aligned storage through std::allocator, then records the successful request. */
    [[nodiscard]] auto allocate(size_t count) noexcept -> T* {
        const auto bytes = checked_bytes(count, "allocate");
        if (bytes > std::numeric_limits<size_t>::max() - accounting_->allocated_bytes_) {
            utils::report_fatal_error("CountingAllocator", "allocate", "allocated byte count overflow",
                count, sizeof(T), accounting_->allocated_bytes_);
        }

        T* allocation = nullptr;
        try {
            allocation = std::allocator<T>{}.allocate(count);
        } catch (const std::exception& error) {
            utils::report_fatal_error("CountingAllocator", "allocate", "allocation failed", count,
                sizeof(T), accounting_->allocated_bytes_, error.what());
        } catch (...) {
            utils::report_fatal_error("CountingAllocator", "allocate",
                "allocation failed with a non-standard exception", count, sizeof(T),
                accounting_->allocated_bytes_);
        }

        accounting_->allocated_bytes_ += bytes;
        return allocation;
    }

    /**
     * @brief Releases storage, then deducts its original request bytes exactly once.
     * @pre allocation is a live pointer from an equal allocator of this type, with the original count.
     *
     * As with std::allocator, the caller must not free an allocation twice. The byte counter does not
     * maintain a pointer registry and cannot diagnose every invalid pointer or mismatched count.
     */
    auto deallocate(T* allocation, size_t count) noexcept -> void {
        const auto bytes = checked_bytes(count, "deallocate");
        if (bytes > accounting_->allocated_bytes_) {
            utils::report_fatal_error("CountingAllocator", "deallocate", "allocated byte count underflow",
                count, sizeof(T), accounting_->allocated_bytes_);
        }

        std::allocator<T>{}.deallocate(allocation, count);
        accounting_->allocated_bytes_ -= bytes;
    }

    /** @brief Allocators are interchangeable only within the same accounting domain. */
    template <typename U>
    [[nodiscard]] auto operator==(const CountingAllocator<U>& other) const noexcept -> bool {
        return accounting_ == other.accounting_;
    }

private:
    template <typename U>
    friend class CountingAllocator;

    [[nodiscard]] auto checked_bytes(size_t count, const char* operation) const noexcept -> size_t {
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            utils::report_fatal_error("CountingAllocator", operation, "request byte count overflow",
                count, sizeof(T), accounting_->allocated_bytes_);
        }
        return count * sizeof(T);
    }

    MemoryAccounting* accounting_;
};

}  // namespace cracking_lsm
