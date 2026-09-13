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

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include <emds-toolkit/common/byte_view.hpp>
#include <emds-toolkit/common/requires.hpp>

#include <cracking-lsm/run/codec/meta_codec/meta_codec.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>

namespace cracking_lsm {

/**
 * @brief Encodes KVEntry metadata field-by-field without persisting native struct padding.
 * @tparam KeyOnly Skips all payload bytes when true, matching EntryMeta<KeyOnly>.
 */
template <bool KeyOnly = false>
class NativeMetaCodec {
public:
    /** @brief Non-owning read-only view over encoded metadata bytes. */
    using ByteViewT = emds::common::ByteView;

    /** @brief Non-owning writable view over output metadata bytes. */
    using MutBytesViewT = emds::common::MutBytesViewT;

    /** @brief Number of bytes occupied by one encoded metadata item. */
    static constexpr std::size_t EncodedBytes = EntryMeta<KeyOnly>::PhysicalBytes;

    /** @brief Encodes one metadata item into caller-owned output bytes. */
    static auto encode(const EntryMeta<KeyOnly>& metadata, MutBytesViewT output) -> void {
        emds::common::require_argument(output.size() >= EncodedBytes,
            "NativeMetaCodec output is smaller than one encoded metadata item");
        std::memcpy(output.data(), &metadata.version_and_kind, sizeof(std::uint64_t));
        if constexpr (!KeyOnly) {
            std::memcpy(output.data() + sizeof(std::uint64_t), &metadata.payload_ref, sizeof(PayloadRefT));
        }
    }

    /** @brief Decodes one metadata item from potentially unaligned native bytes. */
    [[nodiscard]] static auto decode(ByteViewT input) -> EntryMeta<KeyOnly> {
        emds::common::require_argument(input.size() >= EncodedBytes,
            "NativeMetaCodec input is smaller than one encoded metadata item");
        auto metadata = EntryMeta<KeyOnly>{
            .version_and_kind = decode_native<std::uint64_t>(input.data()),
            .payload_ref = {},
        };
        if constexpr (!KeyOnly) {
            metadata.payload_ref = decode_native<PayloadRefT>(input.data() + sizeof(std::uint64_t));
        }
        return metadata;
    }

    /** @brief Encodes metadata items consecutively without persisting struct padding. */
    static auto encode_batch(std::span<const EntryMeta<KeyOnly>> meta_items, MutBytesViewT output) -> void {
        const auto required_bytes = batch_bytes(meta_items.size());
        emds::common::require_argument(output.size() >= required_bytes,
            "NativeMetaCodec output is smaller than the encoded metadata batch");
        for (std::size_t i = 0; i < meta_items.size(); ++i) {
            const auto item_offset = i * EncodedBytes;
            std::memcpy(output.data() + item_offset,
                &meta_items[i].version_and_kind, sizeof(std::uint64_t));
            if constexpr (!KeyOnly) {
                std::memcpy(output.data() + item_offset + sizeof(std::uint64_t),
                    &meta_items[i].payload_ref, sizeof(PayloadRefT));
            }
        }
    }

    /** @brief Decodes consecutive metadata bytes into caller-owned metadata objects. */
    static auto decode_batch(ByteViewT input, std::span<EntryMeta<KeyOnly>> meta_items) -> void {
        const auto required_bytes = batch_bytes(meta_items.size());
        emds::common::require_argument(input.size() >= required_bytes,
            "NativeMetaCodec input is smaller than the encoded metadata batch");
        for (std::size_t i = 0; i < meta_items.size(); ++i) {
            const auto item_offset = i * EncodedBytes;
            meta_items[i].version_and_kind = decode_native<std::uint64_t>(input.data() + item_offset);
            if constexpr (!KeyOnly) {
                meta_items[i].payload_ref = decode_native<PayloadRefT>(
                    input.data() + item_offset + sizeof(std::uint64_t));
            }
        }
    }

private:
    /** @brief Computes the encoded byte count of a batch and rejects size_t overflow. */
    [[nodiscard]] static auto batch_bytes(std::size_t num_items) -> std::size_t {
        emds::common::require_argument(num_items <= std::numeric_limits<std::size_t>::max() / EncodedBytes,
            "NativeMetaCodec encoded metadata batch size overflows size_t");
        return num_items * EncodedBytes;
    }

    /** @brief Decodes one native physical value without requiring input alignment. */
    template <typename PhysicalT>
    [[nodiscard]] static auto decode_native(const std::byte* input) noexcept -> PhysicalT {
        std::array<std::byte, sizeof(PhysicalT)> bytes{};
        std::memcpy(bytes.data(), input, bytes.size());
        return std::bit_cast<PhysicalT>(bytes);
    }
};

static_assert(MetaCodec<NativeMetaCodec<>>);
static_assert(MetaCodec<NativeMetaCodec<true>, true>);

}  // namespace cracking_lsm
