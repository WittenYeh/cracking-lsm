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

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>

#include <emds-toolkit/common/byte_view.hpp>
#include <emds-toolkit/common/requires.hpp>

#include <cracking-lsm/run/block/block_descriptor.hpp>
#include <cracking-lsm/run/codec/key_codec/key_codec.hpp>
#include <cracking-lsm/run/codec/key_codec/native_key_codec.hpp>
#include <cracking-lsm/run/codec/meta_codec/meta_codec.hpp>
#include <cracking-lsm/run/codec/meta_codec/native_meta_codec.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm {

/** @brief Defines and operates on the KeyBlock/MetadataBlock physical representation. */
template <
    PhysicalKey KeyT,
    bool KeyOnly = false,
    typename KeyCodecT = NativeKeyCodec<KeyT>,
    typename MetaCodecT = NativeMetaCodec<KeyOnly>>
requires KeyCodec<KeyCodecT, KeyT> && MetaCodec<MetaCodecT, KeyOnly>
class BlockFormat {
public:
    /** @brief Non-owning read-only view over encoded block bytes. */
    using ByteViewT = emds::common::ByteView;

    /** @brief Non-owning writable view over output block bytes. */
    using MutBytesViewT = emds::common::MutBytesViewT;
    using EntryT = KVEntry<KeyT, KeyOnly>;

    /** @brief Number of encoded bytes occupied by one key. */
    static constexpr std::size_t KeyBytes = KeyCodecT::EncodedBytes;

    /** @brief Number of encoded bytes occupied by one metadata item. */
    static constexpr std::size_t MetaBytes = MetaCodecT::EncodedBytes;

    static_assert(KeyBytes != 0);
    static_assert(MetaBytes != 0);
    static_assert(KeyBytes <= std::numeric_limits<std::size_t>::max() - MetaBytes);

    /** @brief Number of encoded bytes conservatively required by one complete entry. */
    static constexpr std::size_t EntryBytes = KeyBytes + MetaBytes;

    /** @brief Returns the fixed-width entry capacity of a block. */
    [[nodiscard]] static constexpr auto max_entries(std::size_t block_bytes) noexcept
        -> std::size_t {
        return block_bytes / EntryBytes;
    }

    /** @brief Describes the KeyBlock followed by MetadataBlock and zero-padding layout. */
    [[nodiscard]] static auto describe_block(std::size_t num_entries, std::size_t block_bytes)
        -> BlockDescriptor {
        emds::common::require_argument(num_entries <= max_entries(block_bytes),
            "BlockFormat entry count exceeds block capacity");

        const auto encoded_keys_bytes = num_entries * KeyBytes;
        const auto meta_offset = encoded_keys_bytes;
        return BlockDescriptor{
            .encoding_strategy = KeyCodecT::EncodingStrategy,
            .num_entries = num_entries,
            .encoded_keys_bytes = encoded_keys_bytes,
            .meta_offset = meta_offset,
            .encoded_block_bytes = meta_offset + num_entries * MetaBytes,
        };
    }

    /** @brief Validates that a descriptor matches this layout and the loaded block boundary. */
    static auto validate(ByteViewT block, const BlockDescriptor& descriptor) -> void {
        emds::common::require_argument(descriptor.encoding_strategy == KeyCodecT::EncodingStrategy,
            "BlockFormat descriptor uses a different key encoding strategy");
        emds::common::require_argument(descriptor.num_entries <= max_entries(block.size()),
            "BlockFormat entry count exceeds block capacity");
        emds::common::require_argument(
            descriptor.encoded_keys_bytes == descriptor.num_entries * KeyBytes,
            "BlockFormat descriptor has an invalid encoded key size");
        emds::common::require_argument(
            descriptor.meta_offset == descriptor.encoded_keys_bytes,
            "BlockFormat metadata does not immediately follow its key block");
        emds::common::require_argument(
            descriptor.encoded_block_bytes ==
                descriptor.meta_offset + descriptor.num_entries * MetaBytes,
            "BlockFormat descriptor has an invalid encoded block size");
        emds::common::require_argument(descriptor.encoded_block_bytes <= block.size(),
            "BlockFormat descriptor exceeds the loaded block boundary");
    }

    /**
     * @brief Encodes entries into one zero-padded headerless data block.
     *
     * @return The external descriptor required to read the encoded block.
     * @throws std::invalid_argument If an entry kind or the output size is invalid.
     */
    [[nodiscard]] static auto encode_block(
        std::span<const EntryT> entries, MutBytesViewT block
    ) -> BlockDescriptor {
        const auto descriptor = describe_block(entries.size(), block.size());
        validate_entries(entries);

        std::fill(block.begin(), block.end(), std::byte{0});
        for (std::size_t i = 0; i < entries.size(); ++i) {
            KeyCodecT::encode(entries[i].key, block.subspan(key_offset(i), KeyBytes));
            MetaCodecT::encode(entries[i].metadata,
                block.subspan(meta_offset(descriptor, i), MetaBytes));
        }
        return descriptor;
    }

    /**
     * @brief Appends entries by growing the arrays described by external_descriptor.
     *
     * Existing metadata is moved once, then new keys and metadata are written into their final
     * positions. The complete input and descriptor are validated before block bytes are changed.
     *
     * @return The updated external descriptor.
     */
    [[nodiscard]] static auto append_block(
        std::span<const EntryT> entries,
        MutBytesViewT block,
        const BlockDescriptor& external_descriptor
    ) -> BlockDescriptor {
        validate(block, external_descriptor);
        emds::common::require_argument(
            entries.size() <= max_entries(block.size()) - external_descriptor.num_entries,
            "Run data block does not have enough remaining capacity");
        validate_entries(entries);
        if (entries.empty()) {
            return external_descriptor;
        }

        const auto old_num_entries = external_descriptor.num_entries;
        const auto new_num_entries = old_num_entries + entries.size();
        const auto new_descriptor = describe_block(new_num_entries, block.size());
        const auto old_meta_bytes = old_num_entries * MetaBytes;

        std::memmove(
            block.data() + new_descriptor.meta_offset,
            block.data() + external_descriptor.meta_offset,
            old_meta_bytes);

        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto output_index = old_num_entries + i;
            KeyCodecT::encode(entries[i].key,
                block.subspan(key_offset(output_index), KeyBytes));
            MetaCodecT::encode(entries[i].metadata,
                block.subspan(meta_offset(new_descriptor, output_index), MetaBytes));
        }

        std::fill(block.begin() + static_cast<std::ptrdiff_t>(new_descriptor.encoded_block_bytes),
            block.end(), std::byte{0});
        return new_descriptor;
    }

    /** @brief Returns the byte offset of key i from the beginning of a block. */
    [[nodiscard]] static constexpr auto key_offset(std::size_t i) noexcept -> std::size_t {
        return i * KeyBytes;
    }

    /** @brief Returns the byte offset of metadata item i from the beginning of a block. */
    [[nodiscard]] static constexpr auto meta_offset(
        const BlockDescriptor& descriptor, std::size_t i
    ) noexcept -> std::size_t {
        return descriptor.meta_offset + i * MetaBytes;
    }

private:
    /** @brief Rejects entries whose packed metadata contains an unsupported kind. */
    static auto validate_entries(std::span<const EntryT> entries) -> void {
        for (const auto& entry : entries) {
            emds::common::require_argument(entry.metadata.is_valid_kind(),
                "BlockFormat cannot encode an invalid entry kind");
        }
    }
};

}  // namespace cracking_lsm
