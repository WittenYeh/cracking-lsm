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

#include <cracking-lsm/run/codec/key_codec/key_encoding_strategy.hpp>
#include <cracking-lsm/run/codec/key_codec/key_codec.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>

namespace cracking_lsm {

/** @brief Encodes physical keys using their fixed-width native object representation. */
template <PhysicalKey KeyT>
class NativeKeyCodec {
public:
    /** @brief Non-owning read-only view over encoded key bytes. */
    using ByteViewT = emds::common::ByteView;

    /** @brief Non-owning writable view over output key bytes. */
    using MutBytesViewT = emds::common::MutBytesViewT;

    /** @brief Encoding strategy reported in the enclosing runtime block descriptor. */
    static constexpr KeyEncodingStrategyT EncodingStrategy = KeyEncodingStrategyT::native_fixed;

    /** @brief Number of bytes occupied by one encoded key. */
    static constexpr std::size_t EncodedBytes = sizeof(KeyT);

    /** @brief Encodes one key into caller-owned output bytes. */
    static auto encode(const KeyT& key, MutBytesViewT output) -> void {
        emds::common::require_argument(output.size() >= EncodedBytes,
            "NativeKeyCodec output is smaller than one encoded key");
        std::memcpy(output.data(), &key, EncodedBytes);
    }

    /** @brief Decodes one key from potentially unaligned native bytes. */
    [[nodiscard]] static auto decode(ByteViewT input) -> KeyT {
        emds::common::require_argument(input.size() >= EncodedBytes,
            "NativeKeyCodec input is smaller than one encoded key");
        std::array<std::byte, EncodedBytes> bytes{};
        std::memcpy(bytes.data(), input.data(), EncodedBytes);
        return std::bit_cast<KeyT>(bytes);
    }

    /** @brief Encodes contiguous keys into caller-owned output without per-key allocation. */
    static auto encode_batch(std::span<const KeyT> keys, MutBytesViewT output) -> void {
        const auto required_bytes = batch_bytes(keys.size());
        emds::common::require_argument(output.size() >= required_bytes,
            "NativeKeyCodec output is smaller than the encoded key batch");
        if (required_bytes != 0) {
            std::memcpy(output.data(), keys.data(), required_bytes);
        }
    }

    /** @brief Decodes contiguous native bytes into caller-owned key objects. */
    static auto decode_batch(ByteViewT input, std::span<KeyT> keys) -> void {
        const auto required_bytes = batch_bytes(keys.size());
        emds::common::require_argument(input.size() >= required_bytes,
            "NativeKeyCodec input is smaller than the encoded key batch");
        if (required_bytes != 0) {
            std::memcpy(keys.data(), input.data(), required_bytes);
        }
    }

private:
    /** @brief Computes the encoded byte count of a batch and rejects size_t overflow. */
    [[nodiscard]] static auto batch_bytes(std::size_t num_keys) -> std::size_t {
        emds::common::require_argument(num_keys <= std::numeric_limits<std::size_t>::max() / EncodedBytes,
            "NativeKeyCodec encoded key batch size overflows size_t");
        return num_keys * EncodedBytes;
    }
};

static_assert(KeyCodec<NativeKeyCodec<std::uint64_t>, std::uint64_t>);

}  // namespace cracking_lsm
