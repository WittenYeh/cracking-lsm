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
#include <cstddef>
#include <span>

#include <emds-toolkit/common/byte_view.hpp>

#include <cracking-lsm/kv_entry/entry_meta.hpp>

namespace cracking_lsm {

/** @brief Compile-time interface for fixed-width EntryMeta codecs with matching payload mode. */
template <typename CodecT, bool KeyOnly = false>
concept MetaCodec = requires(
    const EntryMeta<KeyOnly>& metadata,
    std::span<const EntryMeta<KeyOnly>> meta_items,
    emds::common::MutBytesViewT output,
    emds::common::ByteView input,
    std::span<EntryMeta<KeyOnly>> decoded_meta
) {
    requires (CodecT::EncodedBytes > 0);
    { CodecT::EncodedBytes } -> std::convertible_to<std::size_t>;
    { CodecT::encode(metadata, output) } -> std::same_as<void>;
    { CodecT::decode(input) } -> std::same_as<EntryMeta<KeyOnly>>;
    { CodecT::encode_batch(meta_items, output) } -> std::same_as<void>;
    { CodecT::decode_batch(input, decoded_meta) } -> std::same_as<void>;
};

}  // namespace cracking_lsm
