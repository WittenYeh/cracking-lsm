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
#include <cracking-lsm/engine/op_result/predecessor_result.hpp>
#include <cracking-lsm/memtable/memtable_impl.hpp>

namespace cracking_lsm {

/**
 * @brief Move-only in-memory KVEntry buffer with shared B-tree node accounting.
 *
 * Public insertion accepts one complete entry before checking the node-allocation threshold.
 * A full Memtable retains its entries, remains queryable, and rejects further insertions.
 * Snapshot queries return owned entry copies that remain valid after insertion, movement, or destruction.
 * Instances are used by one thread at a time.
 * Observation, insertion, and queries require an owned implementation;
 * a moved-from instance may only be destroyed or assigned a new Memtable.
 * @tparam KeyComparatorT Const-callable ordering with non-throwing comparison and copy construction.
 * @tparam KeyOnly Omits entry payload references when true; defaults to false.
 */
template <PhysicalKey KeyT, KeyComparator<KeyT> KeyComparatorT = std::less<>, bool KeyOnly = false>
requires std::is_nothrow_copy_constructible_v<KeyComparatorT>
class Memtable {
public:
    using EntryT = KVEntry<KeyT, KeyOnly>;

    /**
     * @brief Creates an empty B-tree with zero allocated node bytes and a positive flush threshold.
     *
     * The fixed-size owner allocation is excluded from allocated_bytes(). The comparator is copied
     * into that owner; creation neither allocates tree nodes nor compares keys.
     * @throws std::invalid_argument If memtable_bytes is zero.
     * @throws std::bad_alloc If the owner allocation fails before the B-tree is constructed.
     */
    [[nodiscard]] static auto create(MemtableOptions options,
        const KeyComparatorT& comparator = KeyComparatorT{}) -> Memtable {
        if (options.memtable_bytes == 0) {
            throw std::invalid_argument("Memtable requires memtable_bytes greater than zero");
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
     * An entry that reaches or crosses memtable_bytes() is accepted and sets flush_required().
     * Later valid entries return memtable_full without changing the tree or its accounting.
     * Input and count validation also apply to a full Memtable. Results contain post-operation counts.
     * @throws std::invalid_argument If the packed entry kind is unsupported.
     * @throws std::overflow_error If adding one entry would overflow the physical count.
     * @pre This Memtable still owns its implementation.
     */
    [[nodiscard]] auto insert(const EntryT& entry) -> InsertionResult {
        if (flush_required()) {
            impl_->validate_insertion(entry);
            return InsertionResult{
                InsertionState::memtable_full, size(), allocated_bytes(), flush_required()};
        }

        impl_->insert_entry(entry);
        impl_->flush_required = allocated_bytes() >= memtable_bytes();
        return InsertionResult{InsertionState::inserted, size(), allocated_bytes(), flush_required()};
    }

    /**
     * @brief Finds the latest visible entry for a comparator-equivalent key.
     *
     * Returns a value, a tombstone, or not_found. A tombstone wins over a value at the same version.
     * The result owns its entry copy; the query does not change node accounting or the flush flag.
     * @throws std::invalid_argument If read_version exceeds EntryMeta<KeyOnly>::MaxVersion.
     * @pre This Memtable still owns its implementation.
     */
    [[nodiscard]] auto lookup(const KeyT& key, VersionT read_version = EntryMeta<KeyOnly>::MaxVersion) const
        -> LookupResult<KeyT, KeyOnly> {
        const auto entry = impl_->lookup_entry(key, read_version);
        return entry ? LookupResult<KeyT, KeyOnly>{*entry} : LookupResult<KeyT, KeyOnly>{};
    }

    /**
     * @brief Finds the greatest visible, undeleted key strictly before key in comparator order.
     *
     * Excludes the entire comparator-equivalent query group. Each preceding group is resolved to its
     * latest visible entry before checking for deletion, so a tombstone never exposes an older value.
     * The result owns its entry copy; the query does not change node accounting or the flush flag.
     * @throws std::invalid_argument If read_version exceeds EntryMeta<KeyOnly>::MaxVersion.
     * @pre This Memtable still owns its implementation.
     */
    [[nodiscard]] auto predecessor(const KeyT& key,
        VersionT read_version = EntryMeta<KeyOnly>::MaxVersion) const -> PredecessorResult<KeyT, KeyOnly> {
        const auto entry = impl_->predecessor_entry(key, read_version);
        return entry ? PredecessorResult<KeyT, KeyOnly>{*entry} : PredecessorResult<KeyT, KeyOnly>{};
    }

    /** @brief Returns the B-tree's physical entry count, including any duplicate entries. */
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return impl_->entries.size();
    }

    /** @brief Reports whether the B-tree contains no entries. */
    [[nodiscard]] auto empty() const noexcept -> bool {
        return impl_->entries.empty();
    }

    /** @brief Returns live B-tree allocation bytes, excluding the fixed-size owner. */
    [[nodiscard]] auto allocated_bytes() const noexcept -> std::size_t {
        return impl_->accounting.allocated_bytes();
    }

    /** @brief Returns the configured positive node-allocation threshold. */
    [[nodiscard]] auto memtable_bytes() const noexcept -> std::size_t {
        return impl_->options.memtable_bytes;
    }

    /** @brief Reports whether an accepted insertion reached the threshold and further writes are blocked. */
    [[nodiscard]] auto flush_required() const noexcept -> bool {
        return impl_->flush_required;
    }

private:
    using ImplT = MemtableImpl<KeyT, KeyComparatorT, KeyOnly>;

    explicit Memtable(std::unique_ptr<ImplT> impl) noexcept : impl_(std::move(impl)) {}

    std::unique_ptr<ImplT> impl_;
};

}  // namespace cracking_lsm
