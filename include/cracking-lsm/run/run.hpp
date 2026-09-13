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
#include <cracking-lsm/run/block/block_descriptor.hpp>
#include <cracking-lsm/run/block/block_format.hpp>
#include <cracking-lsm/run/block/block_view.hpp>
#include <cracking-lsm/run/codec/key_codec/key_encoding_strategy.hpp>
#include <cracking-lsm/run/codec/key_codec/key_codec.hpp>
#include <cracking-lsm/run/codec/key_codec/native_key_codec.hpp>
#include <cracking-lsm/run/codec/meta_codec/meta_codec.hpp>
#include <cracking-lsm/run/codec/meta_codec/native_meta_codec.hpp>
#include <cracking-lsm/run/op_result.hpp>
#include <cracking-lsm/run/run_file.hpp>
#include <cracking-lsm/run/run_state.hpp>
#include <cracking-lsm/kv_entry/entry_comparator.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm {

/** @brief File-resident Run lifecycle and query interface, implemented in later steps. */
template <PhysicalKey KeyT, typename KeyComparatorT = std::less<KeyT>, bool KeyOnly = false>
requires KeyComparator<KeyComparatorT, KeyT>
class Run;

}  // namespace cracking_lsm
