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

/** @brief An owned point-lookup result; a visible tombstone remains a successful match. */
template <PhysicalKey KeyT, bool KeyOnly = false>
class LookupResult final : public OpResult {
public:
    using EntryT = KVEntry<KeyT, KeyOnly>;

    /** @brief Creates a result with no visible entry. */
    LookupResult() = default;

    /** @brief Copies a visible entry into the result, rejecting an invalid entry kind. */
    explicit LookupResult(EntryT entry) : entry_(std::move(entry)) {
        if (!entry_->metadata.is_valid_kind()) {
            throw std::invalid_argument("LookupResult requires a valid or tombstone entry kind");
        }
    }

    [[nodiscard]] auto operation() const noexcept -> OpKind override {
        return OpKind::lookup;
    }

    /** @brief Derives visibility from the entry, including value hits in key-only mode. */
    [[nodiscard]] auto state() const noexcept -> QueryState {
        if (!entry_) {
            return QueryState::not_found;
        }
        return entry_->kind() == EntryKindT::tombstone ? QueryState::tombstone : QueryState::value;
    }

    /** @brief Returns the owned entry; its lifetime is that of this result, not the source component. */
    [[nodiscard]] auto entry() const noexcept -> const std::optional<EntryT>& {
        return entry_;
    }

private:
    std::optional<EntryT> entry_;
};

}  // namespace cracking_lsm
