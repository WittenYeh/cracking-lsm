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

#include <functional>
#include <type_traits>

#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm {

/**
 * @brief Orders KVEntry objects by key and then by descending packed version-and-kind.
 * Comparisons convert results to bool before logical operations, matching KeyComparator's noexcept contract.
 */
template <PhysicalKey KeyT, KeyComparator<KeyT> KeyComparatorT, bool KeyOnly = false>
class EntryComparator {
public:
    using EntryT = KVEntry<KeyT, KeyOnly>;
    using is_transparent = void;

    /** @brief Copies the user-supplied key comparator without invoking its move constructor. */
    explicit EntryComparator(const KeyComparatorT& key_comparator = KeyComparatorT{})
        noexcept(std::is_nothrow_copy_constructible_v<KeyComparatorT>) : key_comparator_(key_comparator) {}

    // Rvalues also use these copies, preserving the comparator's copy exception guarantees.
    EntryComparator(const EntryComparator&) = default;
    auto operator=(const EntryComparator&) -> EntryComparator& = default;

    /** @brief Orders two entries by user key order, then descending packed version-and-kind. */
    [[nodiscard]] auto operator()(const EntryT& lhs, const EntryT& rhs) const noexcept -> bool {
        if (std::invoke_r<bool>(key_comparator_, lhs.key, rhs.key)) {
            return true;
        }
        if (std::invoke_r<bool>(key_comparator_, rhs.key, lhs.key)) {
            return false;
        }
        return lhs.metadata.version_and_kind > rhs.metadata.version_and_kind;
    }

    /** @brief Compares an entry with a heterogeneous user-key lookup target. */
    [[nodiscard]] auto operator()(const EntryT& lhs, const KeyT& rhs) const noexcept -> bool {
        return std::invoke_r<bool>(key_comparator_, lhs.key, rhs);
    }

    /** @brief Compares a heterogeneous user-key lookup target with an entry. */
    [[nodiscard]] auto operator()(const KeyT& lhs, const EntryT& rhs) const noexcept -> bool {
        return std::invoke_r<bool>(key_comparator_, lhs, rhs.key);
    }

    /** @brief Reports whether two keys are equivalent under the user-supplied ordering. */
    [[nodiscard]] auto keys_equal(const KeyT& lhs, const KeyT& rhs) const noexcept -> bool {
        return !std::invoke_r<bool>(key_comparator_, lhs, rhs) &&
            !std::invoke_r<bool>(key_comparator_, rhs, lhs);
    }

private:
    KeyComparatorT key_comparator_;
};

}  // namespace cracking_lsm
