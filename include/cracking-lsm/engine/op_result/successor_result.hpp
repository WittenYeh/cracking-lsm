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

#include <optional>
#include <stdexcept>
#include <utility>

#include <cracking-lsm/engine/op_result/op_result.hpp>
#include <cracking-lsm/engine/op_result/query_state.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm {

/**
 * @brief An owned public successor result containing a visible value or no entry.
 *
 * The query selects the least visible, undeleted key strictly after its bound in comparator order.
 * This result validates the entry kind; it does not select or verify the successor itself.
 */
template <PhysicalKey KeyT, bool KeyOnly = false>
class SuccessorResult final : public OpResult {
public:
    using EntryT = KVEntry<KeyT, KeyOnly>;

    /** @brief Creates a result with no visible, undeleted successor. */
    SuccessorResult() = default;

    /** @brief Copies a visible value entry, rejecting tombstones and invalid entry kinds. */
    explicit SuccessorResult(EntryT entry) : entry_(std::move(entry)) {
        if (entry_->kind() != EntryKindT::valid) {
            throw std::invalid_argument("SuccessorResult requires a valid entry kind");
        }
    }

    [[nodiscard]] auto operation() const noexcept -> OpKind override {
        return OpKind::successor;
    }

    /** @brief Returns value or not_found; public successor results cannot contain tombstones. */
    [[nodiscard]] auto state() const noexcept -> QueryState {
        return entry_ ? QueryState::value : QueryState::not_found;
    }

    /** @brief Returns the owned successor entry, including its key and visible version. */
    [[nodiscard]] auto entry() const noexcept -> const std::optional<EntryT>& {
        return entry_;
    }

private:
    std::optional<EntryT> entry_;
};

}  // namespace cracking_lsm
