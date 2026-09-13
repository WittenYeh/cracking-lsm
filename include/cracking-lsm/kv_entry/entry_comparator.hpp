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

#include <utility>

#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm {

/** @brief Orders KVEntry objects by key and then by descending packed version-and-kind. */
template <PhysicalKey KeyT, typename KeyComparatorT, bool KeyOnly = false>
requires KeyComparator<KeyComparatorT, KeyT>
class EntryComparator {
public:
    using EntryT = KVEntry<KeyT, KeyOnly>;
    using is_transparent = void;

    /** @brief Creates an entry comparator from the user-supplied key comparator. */
    explicit EntryComparator(KeyComparatorT key_comparator = KeyComparatorT{})
        : key_comparator_(std::move(key_comparator)) {}

    /** @brief Orders two entries as `(key ascending, version-and-kind descending)`. */
    [[nodiscard]] auto operator()(const EntryT& lhs, const EntryT& rhs) const -> bool {
        if (key_comparator_(lhs.key, rhs.key)) {
            return true;
        }
        if (key_comparator_(rhs.key, lhs.key)) {
            return false;
        }
        return lhs.metadata.version_and_kind > rhs.metadata.version_and_kind;
    }

    /** @brief Compares an entry with a heterogeneous user-key lookup target. */
    [[nodiscard]] auto operator()(const EntryT& lhs, const KeyT& rhs) const -> bool {
        return key_comparator_(lhs.key, rhs);
    }

    /** @brief Compares a heterogeneous user-key lookup target with an entry. */
    [[nodiscard]] auto operator()(const KeyT& lhs, const EntryT& rhs) const -> bool {
        return key_comparator_(lhs, rhs.key);
    }

    /** @brief Reports whether two keys are equivalent under the user-supplied ordering. */
    [[nodiscard]] auto keys_equal(const KeyT& lhs, const KeyT& rhs) const -> bool {
        return !key_comparator_(lhs, rhs) && !key_comparator_(rhs, lhs);
    }

private:
    KeyComparatorT key_comparator_;
};

}  // namespace cracking_lsm
