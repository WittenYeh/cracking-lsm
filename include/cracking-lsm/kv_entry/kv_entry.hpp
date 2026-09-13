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

#include <cstdint>
#include <stdexcept>
#include <utility>

#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>

namespace cracking_lsm {

/**
 * @brief A compact versioned key with a compile-time optional value-payload reference.
 *
 * Version and kind share one 64-bit word. The high 56 bits store version and the low 8 bits store
 * kind. Memtables, Runs, queries, and sorting share this entry type; file codecs encode its fields
 * separately without persisting C++ struct padding.
 * @tparam KeyOnly Omits the payload reference when true; defaults to false.
 */
template <PhysicalKey KeyT, bool KeyOnly = false>
struct KVEntry {
    /** @brief Creates an entry that refers to a value payload. */
    [[nodiscard]] static auto make(KeyT key, PayloadRefT payload_ref, VersionT version)
        -> KVEntry requires (!KeyOnly) {
        return KVEntry{
            .key = std::move(key),
            .metadata = EntryMeta<KeyOnly>{
                .version_and_kind = pack(version, EntryKindT::valid),
                .payload_ref = std::move(payload_ref),
            },
        };
    }

    /** @brief Creates a valid key-only entry without a payload reference. */
    [[nodiscard]] static auto make(KeyT key, VersionT version) -> KVEntry requires (KeyOnly) {
        return KVEntry{
            .key = std::move(key),
            .metadata = EntryMeta<KeyOnly>{
                .version_and_kind = pack(version, EntryKindT::valid),
                .payload_ref = {},
            },
        };
    }

    /** @brief Creates an entry that hides older values for the supplied key. */
    [[nodiscard]] static auto tombstone(KeyT key, VersionT version) -> KVEntry {
        return KVEntry{
            .key = std::move(key),
            .metadata = EntryMeta<KeyOnly>{
                .version_and_kind = pack(version, EntryKindT::tombstone),
                .payload_ref = {},
            },
        };
    }

    /** @brief Returns the unpacked 56-bit logical version. */
    [[nodiscard]] constexpr auto version() const noexcept -> VersionT {
        return metadata.version_and_kind >> EntryMeta<KeyOnly>::KindBits;
    }

    /** @brief Returns the EntryKindT stored in the low eight bits. */
    [[nodiscard]] constexpr auto kind() const noexcept -> EntryKindT {
        return static_cast<EntryKindT>(metadata.version_and_kind & EntryMeta<KeyOnly>::KindMask);
    }

    /** @brief Inline physical key representation supplied by the user. */
    KeyT key;

    /** @brief Physical non-key fields paired with this key. */
    EntryMeta<KeyOnly> metadata;

private:
    /** @brief Packs version and kind, rejecting versions that exceed the 56-bit field. */
    [[nodiscard]] static constexpr auto pack(VersionT version, EntryKindT kind) -> std::uint64_t {
        if (version > EntryMeta<KeyOnly>::MaxVersion) {
            throw std::invalid_argument("KVEntry version exceeds its 56-bit physical field");
        }
        return (version << EntryMeta<KeyOnly>::KindBits) | static_cast<std::uint8_t>(kind);
    }
};

}  // namespace cracking_lsm
