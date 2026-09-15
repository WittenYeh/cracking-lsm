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
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include <absl/container/btree_set.h>

#include <cracking-lsm/options.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>
#include <cracking-lsm/kv_entry/entry_comparator.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>
#include <cracking-lsm/memtable/memory_accounting.hpp>

namespace cracking_lsm {

/**
 * @brief Stable B-tree storage with checked insertion and snapshot queries owned by Memtable.
 *
 * The accounting object precedes the B-tree so nodes are released before their accounting state.
 * Moving the owning Memtable transfers its unique_ptr and preserves this object's address.
 */
template <PhysicalKey KeyT, KeyComparator<KeyT> KeyComparatorT, bool KeyOnly>
requires std::is_nothrow_copy_constructible_v<KeyComparatorT>
struct MemtableImpl {
    using EntryT = KVEntry<KeyT, KeyOnly>;
    using EntryComparatorT = EntryComparator<KeyT, KeyComparatorT, KeyOnly>;
    using AllocatorT = CountingAllocator<EntryT>;
    using TreeT = absl::btree_multiset<EntryT, EntryComparatorT, AllocatorT>;

    /** @brief Builds an empty tree after the owning Memtable validates its options. */
    MemtableImpl(MemtableOptions options_value, const KeyComparatorT& comparator)
        : options(options_value),
          entries(EntryComparatorT{comparator}, AllocatorT{accounting}) {}

    /**
     * @brief Checks one entry and the tree's current physical count before any modification.
     * @throws std::invalid_argument If the packed entry kind is unsupported.
     * @throws std::overflow_error If adding one entry would overflow the physical count.
     */
    auto validate_insertion(const EntryT& entry) const -> void {
        if (!entry.metadata.is_valid_kind()) {
            throw std::invalid_argument("Memtable insertion requires a valid entry kind");
        }
        if (entries.size() == std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("Memtable insertion overflows the physical entry count");
        }
    }

    /**
     * @brief Validates and copies one physical entry into the B-tree, including equivalent entries.
     *
     * The tree owns its entry count; CountingAllocator tracks all node growth and releases.
     * Capacity policy and InsertionResult belong to Memtable::insert.
     * Validation errors propagate before modification. Node allocation failures terminate through
     * the allocator, and comparisons follow the shared noexcept contract.
     */
    auto insert_entry(const EntryT& entry) -> void {
        validate_insertion(entry);
        static_cast<void>(entries.insert(entry));
    }

    /**
     * @brief Returns an owned copy of the latest visible entry for a comparator-equivalent key.
     *
     * The compound probe skips newer versions in logarithmic time and selects tombstones first
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
     * @brief Returns the latest visible value of the greatest undeleted key strictly before before_key.
     *
     * After one tree search, resolves each preceding key group to its latest visible entry.
     * Invisible groups and groups deleted by a visible tombstone are skipped in the same traversal.
     * Returns an owned value entry or an empty optional; a tombstone is never returned.
     * @throws std::invalid_argument If read_version exceeds the physical version field.
     */
    [[nodiscard]] auto predecessor_entry(const KeyT& before_key, VersionT read_version) const
        -> std::optional<EntryT> {
        validate_read_version(read_version);
        auto position = entries.lower_bound(before_key);
        const auto first = entries.begin();
        const auto comparator = entries.key_comp();

        while (position != first) {
            const auto group_last = std::prev(position);
            const auto& group_key = group_last->key;
            auto visible = entries.end();
            do {
                --position;
                if (position->version() <= read_version) {
                    // Reverse traversal visits older versions and smaller kinds first.
                    visible = position;
                }
            } while (position != first && comparator.keys_equal(std::prev(position)->key, group_key));

            if (visible != entries.end() && visible->kind() == EntryKindT::valid) {
                return *visible;
            }
        }
        return std::nullopt;
    }

    MemtableOptions options;
    MemoryAccounting accounting;
    TreeT entries;
    bool flush_required = false;

private:
    /** @brief Rejects out-of-range snapshots before any tree search, including on an empty tree. */
    static auto validate_read_version(VersionT read_version) -> void {
        if (read_version > EntryMeta<KeyOnly>::MaxVersion) {
            throw std::invalid_argument("Memtable query read_version exceeds its 56-bit physical field");
        }
    }
};

}  // namespace cracking_lsm
