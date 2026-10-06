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
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include <oneapi/tbb/concurrent_set.h>

#include <cracking-lsm/options.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>
#include <cracking-lsm/kv_entry/entry_comparator.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>
#include <cracking-lsm/utils/error.hpp>

namespace cracking_lsm {

/**
 * @brief Concurrent ordered storage with checked insertion and version-filtered queries.
 *
 * Moving the owning Memtable transfers its unique_ptr and preserves this object's address.
 */
template <PhysicalKey KeyT, KeyComparator<KeyT> KeyComparatorT, bool KeyOnly>
requires std::is_nothrow_copy_constructible_v<KeyComparatorT>
struct MemtableImpl {
    using EntryT = KVEntry<KeyT, KeyOnly>;
    static_assert(alignof(EntryT) <= alignof(std::max_align_t),
        "Memtable does not support over-aligned entry types with the default TBB allocator");

    using EntryComparatorT = EntryComparator<KeyT, KeyComparatorT, KeyOnly>;
    using IndexT = oneapi::tbb::concurrent_multiset<EntryT, EntryComparatorT>;

    /** @brief Builds an empty index after the owning Memtable validates its options. */
    MemtableImpl(MemtableOptions options_value, const KeyComparatorT& comparator)
        : options(options_value), entries(EntryComparatorT{comparator}) {}

    /**
     * @brief Checks one entry and the index's reserved physical count before any modification.
     * @throws std::invalid_argument If the packed entry kind is unsupported.
     * @throws std::overflow_error If adding one entry would overflow the physical count.
     */
    auto validate_insertion(const EntryT& entry) const -> void {
        if (!entry.metadata.is_valid_kind()) {
            throw std::invalid_argument("Memtable insertion requires a valid entry kind");
        }
        if (reserved_entries.load(std::memory_order_relaxed) == std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("Memtable insertion overflows the physical entry count");
        }
    }

    /**
     * @brief Validates and copies one physical entry into the concurrent index, including duplicates.
     *
     * A reservation prevents concurrent insertions from overflowing the physical count.
     * Completed insertions have a separate O(1) count; oneTBB's size() aggregates per-thread counts.
     * Capacity policy and InsertionResult belong to Memtable::insert.
     * Validation errors propagate before modification. Index insertion failures are reported here
     * before termination, and comparisons follow the shared noexcept contract.
     */
    auto insert_entry(const EntryT& entry) -> void {
        validate_insertion(entry);
        auto reserved = reserved_entries.load(std::memory_order_relaxed);
        do {
            if (reserved == std::numeric_limits<std::size_t>::max()) {
                throw std::overflow_error("Memtable insertion overflows the physical entry count");
            }
        } while (!reserved_entries.compare_exchange_weak(reserved, reserved + 1, std::memory_order_relaxed));

        try {
            static_cast<void>(entries.insert(entry));
        } catch (const std::exception& error) {
            utils::report_fatal_error("Memtable", "insert", "index insertion failed", 1,
                sizeof(EntryT), error.what());
        } catch (...) {
            utils::report_fatal_error("Memtable", "insert",
                "index insertion failed with a non-standard exception", 1, sizeof(EntryT));
        }
        num_entries.fetch_add(1, std::memory_order_release);
    }

    /**
     * @brief Returns an owned copy of the latest visible entry for a comparator-equivalent key.
     *
     * The compound probe skips newer versions in expected logarithmic time and selects tombstones first
     * at equal versions. No visible entry for the requested key yields an empty optional.
     * @throws std::invalid_argument If read_version exceeds the physical version field.
     */
    [[nodiscard]] auto lookup_entry(const KeyT& key, VersionT read_version) const -> std::optional<EntryT> {
        validate_read_version(read_version);
        const auto position = entries.lower_bound(EntryT::tombstone(key, read_version));
        if (position == entries.end() || !entries.key_comp().keys_equal(position->key, key)) {
            return std::nullopt;
        }
        return *position;
    }

    /**
     * @brief Returns the latest visible value of the least undeleted key strictly after after_key.
     *
     * After one upper-bound search, visits later keys in user-comparator order using forward iteration.
     * Invisible groups and groups deleted by a visible tombstone are skipped in the same traversal.
     * Returns an owned value entry or an empty optional; a tombstone is never returned.
     * @throws std::invalid_argument If read_version exceeds the physical version field.
     */
    [[nodiscard]] auto successor_entry(const KeyT& after_key, VersionT read_version) const
        -> std::optional<EntryT> {
        validate_read_version(read_version);
        auto position = entries.upper_bound(after_key);
        const auto last = entries.end();
        const auto comparator = entries.key_comp();

        while (position != last) {
            const auto group_key = position->key;
            while (position != last && comparator.keys_equal(position->key, group_key)) {
                if (position->version() > read_version) {
                    ++position;
                    continue;
                }
                // The first visible entry wins; a tombstone hides all older versions of this key.
                if (position->kind() == EntryKindT::valid) {
                    return *position;
                }
                do {
                    ++position;
                } while (position != last && comparator.keys_equal(position->key, group_key));
                break;
            }
        }
        return std::nullopt;
    }

    /** @brief Immutable physical-entry flush threshold. */
    MemtableOptions options;
    /** @brief Keys in user-comparator order, with versions and kinds descending within each key. */
    IndexT entries;
    /** @brief Completed insertions, including physical duplicates; O(1) to observe. */
    std::atomic<std::size_t> num_entries{0};
    /** @brief Completed and in-flight insertion reservations used to prevent count overflow. */
    std::atomic<std::size_t> reserved_entries{0};
    /** @brief Sticky admission flag; already admitted concurrent insertions may still finish. */
    std::atomic<bool> flush_required{false};

private:
    /** @brief Rejects out-of-range versions before any index search, including on an empty index. */
    static auto validate_read_version(VersionT read_version) -> void {
        if (read_version > EntryMeta<KeyOnly>::MaxVersion) {
            throw std::invalid_argument("Memtable query read_version exceeds its 56-bit physical field");
        }
    }
};

}  // namespace cracking_lsm
