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
#include <utility>

#include <emds-toolkit/common/byte_view.hpp>
#include <emds-toolkit/common/requires.hpp>

#include <cracking-lsm/run/block/block_format.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm {

/** @brief Read-only ordinal view over one descriptor-validated Run data block. */
template <
    PhysicalKey KeyT,
    bool KeyOnly = false,
    typename KeyCodecT = NativeKeyCodec<KeyT>,
    typename MetaCodecT = NativeMetaCodec<KeyOnly>>
requires KeyCodec<KeyCodecT, KeyT> && MetaCodec<MetaCodecT, KeyOnly>
class BlockView {
public:
    /** @brief Non-owning read-only view over loaded block bytes. */
    using ByteViewT = emds::common::ByteView;
    using EntryT = KVEntry<KeyT, KeyOnly>;
    using FormatT = BlockFormat<KeyT, KeyOnly, KeyCodecT, MetaCodecT>;

    /** @brief Number of persisted bytes occupied by one metadata item. */
    static constexpr std::size_t MetaBytes = FormatT::MetaBytes;

    /** @brief Validates a descriptor and creates a non-owning view over loaded block bytes. */
    [[nodiscard]] static auto make(
        ByteViewT loaded_block, const BlockDescriptor& descriptor
    ) -> BlockView {
        FormatT::validate(loaded_block, descriptor);
        return BlockView{loaded_block, descriptor};
    }

    /** @brief Returns the external descriptor validated when this view was created. */
    [[nodiscard]] auto descriptor() const noexcept -> const BlockDescriptor& {
        return descriptor_;
    }

    /** @brief Returns the number of key/metadata pairs in this block. */
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return descriptor_.num_entries;
    }

    /** @brief Decodes the fixed-width key at ordinal i. */
    [[nodiscard]] auto key_at(std::size_t i) const -> KeyT {
        require_ordinal(i);
        return KeyCodecT::decode(
            loaded_block_.subspan(FormatT::key_offset(i), FormatT::KeyBytes));
    }

    /** @brief Decodes the metadata paired with the key at ordinal i. */
    [[nodiscard]] auto meta_at(std::size_t i) const -> EntryMeta<KeyOnly> {
        require_ordinal(i);
        const auto item_offset = FormatT::meta_offset(descriptor_, i);
        return MetaCodecT::decode(loaded_block_.subspan(item_offset, MetaBytes));
    }

    /** @brief Reconstructs the complete logical entry at ordinal i. */
    [[nodiscard]] auto entry_at(std::size_t i) const -> EntryT {
        auto key = key_at(i);
        const auto metadata = meta_at(i);
        emds::common::require_argument(metadata.is_valid_kind(),
            "Run data block contains an invalid entry kind");
        return EntryT{
            .key = std::move(key),
            .metadata = metadata,
        };
    }

private:
    /** @brief Creates a non-owning view over descriptor-validated loaded block bytes. */
    BlockView(ByteViewT loaded_block, BlockDescriptor descriptor) noexcept
        : loaded_block_(loaded_block), descriptor_(descriptor) {}

    /** @brief Rejects an ordinal outside the descriptor-declared entry range. */
    auto require_ordinal(std::size_t i) const -> void {
        emds::common::require_argument(i < descriptor_.num_entries,
            "Run data-block ordinal is outside the entry range");
    }

    /** @brief Non-owning view of the loaded bytes interpreted by this object. */
    ByteViewT loaded_block_;

    /** @brief Runtime layout description copied into this view. */
    BlockDescriptor descriptor_;
};

}  // namespace cracking_lsm
