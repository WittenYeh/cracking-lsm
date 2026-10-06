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

#include <cstdint>

namespace cracking_lsm {

/** @brief Operation described by an engine result. */
enum class OpKind : std::uint8_t {
    insertion,
    lookup,
    successor,
    append,
};

/**
 * @brief Abstract interface shared by operation results from Memtable, Run, and the engine.
 *
 * Concrete results can be returned by value; this interface does not require heap allocation.
 * These polymorphic objects are runtime results, not part of the persisted entry or block format.
 */
class OpResult {
public:
    virtual ~OpResult() = default;

    /** @brief Identifies the operation without merging its operation-specific outcome states. */
    [[nodiscard]] virtual auto operation() const noexcept -> OpKind = 0;

protected:
    OpResult() = default;
    OpResult(const OpResult&) = default;
    OpResult(OpResult&&) = default;
    auto operator=(const OpResult&) -> OpResult& = default;
    auto operator=(OpResult&&) -> OpResult& = default;
};

}  // namespace cracking_lsm
