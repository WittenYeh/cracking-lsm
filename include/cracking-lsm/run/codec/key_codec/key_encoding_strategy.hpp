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

/** @brief Identifies the encoding strategy used for a sequence of physical keys. */
enum class KeyEncodingStrategyT : std::uint8_t {
    /** @brief Fixed-width native object representation without key compression. */
    native_fixed = 1,
};

}  // namespace cracking_lsm
