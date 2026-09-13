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
#include <cstdint>
#include <type_traits>
#include <variant>

namespace cracking_lsm {

/** @brief Globally ordered logical version assigned to an LSM entry. */
using VersionT = std::uint64_t;

/** @brief Eight-byte reference to a value payload stored outside the entry. */
using PayloadRefT = std::uint64_t;

/** @brief Describes whether an entry is valid or deletes an older key. */
enum class EntryKindT : std::uint8_t {
    valid,
    tombstone,
};

/**
 * @brief Physical non-key fields shared by typed entries and file codecs.
 * @tparam KeyOnly Omits payload storage when true, retaining only version and kind.
 */
template <bool KeyOnly = false>
struct EntryMeta {
    /** @brief Number of low bits reserved for EntryKindT. */
    static constexpr std::size_t KindBits = 8;

    /** @brief Mask selecting the low KindBits bits that store EntryKindT. */
    static constexpr std::uint64_t KindMask = (std::uint64_t{1} << KindBits) - 1;

    /** @brief Largest logical version representable by the packed 56-bit version field. */
    static constexpr VersionT MaxVersion = (VersionT{1} << (64 - KindBits)) - 1;

    /** @brief Number of bytes occupied by physical metadata fields, excluding struct padding. */
    static constexpr std::size_t PhysicalBytes = sizeof(std::uint64_t) + (KeyOnly ? 0 : sizeof(PayloadRefT));

    /** @brief Reports whether the packed metadata word contains a supported EntryKindT. */
    [[nodiscard]] constexpr auto is_valid_kind() const noexcept -> bool {
        const auto kind = static_cast<EntryKindT>(version_and_kind & KindMask);
        return kind == EntryKindT::valid || kind == EntryKindT::tombstone;
    }

    /** @brief Packed 56-bit version and 8-bit entry kind. */
    std::uint64_t version_and_kind;

    /** @brief External value reference for a valid entry, or an empty placeholder in key-only mode. */
    [[no_unique_address]] std::conditional_t<KeyOnly, std::monostate, PayloadRefT> payload_ref;
};

static_assert(sizeof(EntryMeta<true>) == EntryMeta<true>::PhysicalBytes,
    "Key-only EntryMeta requires a zero-overhead payload placeholder");
static_assert(sizeof(EntryMeta<false>) == EntryMeta<false>::PhysicalBytes,
    "EntryMeta must contain only the packed word and payload reference");

}  // namespace cracking_lsm
