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

#include <concepts>
#include <type_traits>

namespace cracking_lsm {

/**
 * @brief A fixed-size key whose object representation is its persisted physical representation.
 *
 * The user owns the key schema, byte order, and compatibility across platforms and software
 * versions. Pointer-owning or dynamically allocated C++ types are not physical keys.
 */
template <typename KeyT>
concept PhysicalKey =
    std::copyable<KeyT> &&
    (!std::is_pointer_v<KeyT>) &&
    (!std::is_member_pointer_v<KeyT>) &&
    std::is_trivially_copyable_v<KeyT> &&
    std::is_standard_layout_v<KeyT> &&
    std::has_unique_object_representations_v<KeyT>;

/** @brief Const-callable strict weak ordering with non-throwing invocation and conversion to bool. */
template <typename KeyComparatorT, typename KeyT>
concept KeyComparator =
    PhysicalKey<KeyT> &&
    std::copy_constructible<KeyComparatorT> &&
    std::strict_weak_order<const KeyComparatorT&, const KeyT&, const KeyT&> &&
    std::is_nothrow_invocable_r_v<bool, const KeyComparatorT&, const KeyT&, const KeyT&>;

}  // namespace cracking_lsm
