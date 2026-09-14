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
#include <cracking-lsm/engine/op_result/insertion_state.hpp>

namespace cracking_lsm {

/** @brief Outcome and post-operation counters for a single entry insertion. */
class InsertionResult final : public OpResult {
public:
    /** @brief Records the actual insertion outcome and all post-operation counters. */
    InsertionResult(InsertionState status_value, std::size_t total_entries, std::size_t node_bytes,
        bool requires_flush) noexcept
        : status(status_value), num_total_entries(total_entries), allocated_bytes(node_bytes),
          flush_required(requires_flush) {}

    [[nodiscard]] auto operation() const noexcept -> OpKind override {
        return OpKind::insertion;
    }

    /** @brief Whether this entry was accepted or rejected because the Memtable was already full. */
    InsertionState status;

    /** @brief Physical entry count after the attempt, including duplicates, versions, and tombstones. */
    std::size_t num_total_entries;

    /** @brief Live node allocation bytes after the attempt, using the MemtableOptions accounting scope. */
    std::size_t allocated_bytes;

    /** @brief True for a full Memtable, including an accepted entry that reaches the byte threshold. */
    bool flush_required;
};

}  // namespace cracking_lsm
