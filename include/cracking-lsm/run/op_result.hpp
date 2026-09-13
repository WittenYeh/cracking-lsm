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
#include <optional>
#include <utility>

#include <cracking-lsm/kv_entry/entry_meta.hpp>

namespace cracking_lsm {

/** @brief Result state returned by building and sealed Run lookups. */
enum class LookupState : std::uint8_t {
    not_found,
    value,
    tombstone,
};

/** @brief The newest entry visible to a point lookup at a requested version. */
struct LookupResult {
    /** @brief Creates a result for a key with no visible entry. */
    [[nodiscard]] static auto not_found() noexcept -> LookupResult {
        return LookupResult{};
    }

    /** @brief Creates a result that refers to a visible value payload. */
    [[nodiscard]] static auto value(PayloadRefT payload_ref, VersionT version) -> LookupResult {
        return LookupResult{
            .state = LookupState::value,
            .version = version,
            .payload_ref = std::move(payload_ref),
        };
    }

    /** @brief Creates a result for a visible deletion marker. */
    [[nodiscard]] static auto tombstone(VersionT version) noexcept -> LookupResult {
        return LookupResult{
            .state = LookupState::tombstone,
            .version = version,
            .payload_ref = std::nullopt,
        };
    }

    /** @brief Lookup outcome. */
    LookupState state = LookupState::not_found;

    /** @brief Visible version; meaningful only when @ref state is not `not_found`. */
    VersionT version = 0;

    /** @brief Payload reference; present exactly when @ref state is `value`. */
    std::optional<PayloadRefT> payload_ref = std::nullopt;
};

/** @brief State returned after a complete batch is successfully appended to a building Run. */
struct AppendResult {
    /** @brief Total number of physical entries, including duplicates, after the append. */
    std::size_t num_total_entries = 0;

    /** @brief Whether the building Run reached its threshold and now requires sealing. */
    bool seal_required = false;
};

}  // namespace cracking_lsm
