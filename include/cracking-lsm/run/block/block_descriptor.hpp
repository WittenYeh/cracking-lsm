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

#include <cracking-lsm/run/codec/key_codec/key_encoding_strategy.hpp>

namespace cracking_lsm {

/**
 * @brief Runtime-only description used to interpret a headerless Run data block.
 *
 * A building Run derives this object from run-level counters. A sealed Run stores only the compact
 * block-location fields required by its external index. This native struct is never serialized.
 */
struct BlockDescriptor {
    /** @brief Encoding strategy used by the contiguous key block. */
    KeyEncodingStrategyT encoding_strategy;

    /** @brief Number of key/metadata pairs stored in the data block. */
    std::size_t num_entries;

    /** @brief Number of encoded key bytes from the beginning of the data block. */
    std::size_t encoded_keys_bytes;

    /** @brief Byte offset of the metadata block from the beginning of the data block. */
    std::size_t meta_offset;

    /** @brief Total encoded bytes before zero padding begins. */
    std::size_t encoded_block_bytes;
};

}  // namespace cracking_lsm
