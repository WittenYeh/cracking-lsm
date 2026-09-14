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

#include <functional>

#include <cracking-lsm/options.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>
#include <cracking-lsm/engine/op_result/insertion_result.hpp>
#include <cracking-lsm/engine/op_result/lookup_result.hpp>
#include <cracking-lsm/engine/op_result/predecessor_result.hpp>

namespace cracking_lsm {

/**
 * @brief Independent in-memory KVEntry buffer, defined in later implementation steps.
 * @tparam KeyOnly Omits entry payload references when true; defaults to false.
 */
template <PhysicalKey KeyT, typename KeyComparatorT = std::less<KeyT>, bool KeyOnly = false>
requires KeyComparator<KeyComparatorT, KeyT>
class Memtable;

}  // namespace cracking_lsm
