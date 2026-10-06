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

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <cracking-lsm/options.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>
#include <cracking-lsm/engine/op_result/insertion_result.hpp>
#include <cracking-lsm/engine/op_result/lookup_result.hpp>
#include <cracking-lsm/engine/op_result/successor_result.hpp>
#include <cracking-lsm/memtable/memtable_impl.hpp>

namespace cracking_lsm {

/**
 * @brief Move-only concurrent KVEntry buffer with a physical-entry flush threshold.
 *
 * Public insertion accepts one complete entry before checking the stored-entry threshold.
 * A full Memtable retains its entries, remains queryable, and rejects further insertions.
 * Version-filtered queries return owned copies that survive insertion, movement, or destruction.
 * Insertion, observation and queries may run concurrently. Queries overlapping inserts are weakly
 * consistent; read_version is a version ceiling, not a fixed MVCC snapshot or commit watermark.
 * Moving, assigning, destroying and future erasure require external exclusion of all operations.
 * Observation, insertion, and queries require an owned implementation;
 * a moved-from instance may only be destroyed or assigned a new Memtable.
 * Entry alignment must not exceed alignof(std::max_align_t); the index uses the default TBB allocator.
 * @tparam KeyComparatorT Thread-safe ordering with non-throwing comparison and copy construction.
 * @tparam KeyOnly Omits entry payload references when true; defaults to false.
 */
template <PhysicalKey KeyT, KeyComparator<KeyT> KeyComparatorT = std::less<>, bool KeyOnly = false>
requires std::is_nothrow_copy_constructible_v<KeyComparatorT>
class Memtable {
public:
    using EntryT = KVEntry<KeyT, KeyOnly>;

    /**
     * @brief Creates an empty concurrent index with a positive physical-entry flush threshold.
     *
     * The comparator is copied into the owner; oneTBB allocates nodes on insertion.
     * @throws std::invalid_argument If max_entries is zero.
     * @throws std::bad_alloc If owner or oneTBB bookkeeping allocation fails during construction.
     */
    [[nodiscard]] static auto create(MemtableOptions options,
        const KeyComparatorT& comparator = KeyComparatorT{}) -> Memtable {
        if (options.max_entries == 0) {
            throw std::invalid_argument("Memtable requires max_entries greater than zero");
        }
        return Memtable{std::make_unique<ImplT>(options, comparator)};
    }

    Memtable(const Memtable&) = delete;
    auto operator=(const Memtable&) -> Memtable& = delete;
    Memtable(Memtable&&) noexcept = default;
    auto operator=(Memtable&&) noexcept -> Memtable& = default;
    ~Memtable() = default;

    /**
     * @brief Inserts one physical entry unless an earlier insertion already required a flush.
     *
     * An entry that reaches or crosses max_entries() is accepted and sets flush_required().
     * Duplicates, historical versions, and tombstones each count as one physical entry.
     * Later valid entries return memtable_full without allocating nodes for the rejected entry.
     * Concurrent insertions admitted before the flush flag is observed may finish and exceed the threshold.
     * Input and count validation also apply to a full Memtable. Result fields are post-insertion
     * observations; concurrent operations may change them and they are not one atomic state snapshot.
     * @throws std::invalid_argument If the packed entry kind is unsupported.
     * @throws std::overflow_error If adding one entry would overflow the physical count.
     * @pre This Memtable still owns its implementation.
     */
    [[nodiscard]] auto insert(const EntryT& entry) -> InsertionResult {
        if (flush_required()) {
            impl_->validate_insertion(entry);
            return InsertionResult{InsertionState::memtable_full, size(), flush_required()};
        }

        impl_->insert_entry(entry);
        if (size() >= max_entries()) {
            impl_->flush_required.store(true, std::memory_order_release);
        }
        return InsertionResult{InsertionState::inserted, size(), flush_required()};
    }

    /**
     * @brief Finds the latest visible entry for a comparator-equivalent key.
     *
     * Returns a value, a tombstone, or not_found. A tombstone wins over a value at the same version.
     * The result owns its entry copy; the query does not change the entry count or the flush flag.
     * @throws std::invalid_argument If read_version exceeds EntryMeta<KeyOnly>::MaxVersion.
     * @pre This Memtable still owns its implementation.
     */
    [[nodiscard]] auto lookup(const KeyT& key, VersionT read_version = EntryMeta<KeyOnly>::MaxVersion) const
        -> LookupResult<KeyT, KeyOnly> {
        const auto entry = impl_->lookup_entry(key, read_version);
        return entry ? LookupResult<KeyT, KeyOnly>{*entry} : LookupResult<KeyT, KeyOnly>{};
    }

    /**
     * @brief Finds the least visible, undeleted key strictly after key in comparator order.
     *
     * Excludes the entire comparator-equivalent query group. Each following group is resolved to its
     * latest visible entry before checking for deletion, so a tombstone never exposes an older value.
     * The result owns its entry copy; the query does not change the entry count or the flush flag.
     * An ascending comparator finds a natural-order successor; a descending one finds a predecessor.
     * @throws std::invalid_argument If read_version exceeds EntryMeta<KeyOnly>::MaxVersion.
     * @pre This Memtable still owns its implementation.
     */
    [[nodiscard]] auto successor(const KeyT& key,
        VersionT read_version = EntryMeta<KeyOnly>::MaxVersion) const -> SuccessorResult<KeyT, KeyOnly> {
        const auto entry = impl_->successor_entry(key, read_version);
        return entry ? SuccessorResult<KeyT, KeyOnly>{*entry} : SuccessorResult<KeyT, KeyOnly>{};
    }

    /** @brief Returns the number of completed insertions, including physical duplicates. */
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return impl_->num_entries.load(std::memory_order_acquire);
    }

    /** @brief Reports whether no insertions have completed at the time of observation. */
    [[nodiscard]] auto empty() const noexcept -> bool {
        return size() == 0;
    }

    /** @brief Returns the configured positive physical-entry flush threshold. */
    [[nodiscard]] auto max_entries() const noexcept -> std::size_t {
        return impl_->options.max_entries;
    }

    /** @brief Reports whether an accepted insertion reached the threshold and further writes are blocked. */
    [[nodiscard]] auto flush_required() const noexcept -> bool {
        return impl_->flush_required.load(std::memory_order_acquire);
    }

private:
    using ImplT = MemtableImpl<KeyT, KeyComparatorT, KeyOnly>;

    explicit Memtable(std::unique_ptr<ImplT> impl) noexcept : impl_(std::move(impl)) {}

    std::unique_ptr<ImplT> impl_;
};

}  // namespace cracking_lsm
