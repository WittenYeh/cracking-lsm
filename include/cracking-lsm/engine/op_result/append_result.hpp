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

#include <cracking-lsm/engine/op_result/op_result.hpp>

namespace cracking_lsm {

/** @brief Counters after a complete batch was successfully appended to a building Run. */
class AppendResult final : public OpResult {
public:
    /** @brief Records the post-append physical count and the Run's seal threshold state. */
    AppendResult(std::size_t total_entries, bool requires_seal) noexcept
        : num_total_entries(total_entries), seal_required(requires_seal) {}

    [[nodiscard]] auto operation() const noexcept -> OpKind override {
        return OpKind::append;
    }

    /** @brief Total physical entry count after the append, including duplicates. */
    std::size_t num_total_entries;

    /** @brief Whether the building Run reached its threshold and now requires sealing. */
    bool seal_required;
};

}  // namespace cracking_lsm
