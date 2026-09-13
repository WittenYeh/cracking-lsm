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

#include <cracking-lsm/run/codec/key_codec/key_encoding_strategy.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>

namespace cracking_lsm {

/** @brief Compile-time interface for fixed-width key codecs with allocation-free batch APIs. */
template <typename CodecT, typename KeyT>
concept KeyCodec =
    PhysicalKey<KeyT> &&
    requires(
        const KeyT& key,
        std::span<const KeyT> keys,
        emds::common::MutBytesViewT output,
        emds::common::ByteView input,
        std::span<KeyT> decoded_keys
    ) {
        requires (CodecT::EncodedBytes > 0);
        { CodecT::EncodingStrategy } -> std::convertible_to<KeyEncodingStrategyT>;
        { CodecT::EncodedBytes } -> std::convertible_to<std::size_t>;
        { CodecT::encode(key, output) } -> std::same_as<void>;
        { CodecT::decode(input) } -> std::same_as<KeyT>;
        { CodecT::encode_batch(keys, output) } -> std::same_as<void>;
        { CodecT::decode_batch(input, decoded_keys) } -> std::same_as<void>;
    };

}  // namespace cracking_lsm
