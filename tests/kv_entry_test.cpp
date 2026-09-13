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

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string_view>

#include <gtest/gtest.h>

#include <cracking-lsm/kv_entry/entry_comparator.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm {
namespace {

using KeyT = std::uint64_t;
using EntryT = KVEntry<KeyT>;

struct InvalidKeyComparator {
    auto operator()(std::string_view lhs, std::string_view rhs) const -> bool {
        return lhs < rhs;
    }
};

static_assert(KeyComparator<std::less<KeyT>, KeyT>);
static_assert(!KeyComparator<InvalidKeyComparator, KeyT>);

/** @brief Verifies key ascending, packed version-and-kind descending, and ignored payload ordering. */
TEST(KVEntryTest, EntryComparatorEstablishesInternalKeyOrder) {
    const EntryComparator<KeyT, std::less<KeyT>> comparator;
    const auto key_one = EntryT::make(1, 10, 4);
    const auto key_two = EntryT::make(2, 10, 4);
    const auto newest = EntryT::make(1, 10, 7);
    const auto oldest = EntryT::make(1, 10, 3);
    auto tombstone = EntryT::tombstone(1, 7);
    tombstone.metadata.payload_ref = 99;
    const auto same_internal_key = EntryT::make(1, 999, 7);

    EXPECT_TRUE(comparator(key_one, key_two));
    EXPECT_TRUE(comparator(newest, oldest));
    EXPECT_TRUE(comparator(tombstone, newest));
    EXPECT_FALSE(comparator(newest, same_internal_key));
    EXPECT_FALSE(comparator(same_internal_key, newest));
}

/** @brief Verifies the packed 56-bit version boundary and kind round trip. */
TEST(KVEntryTest, KVEntryEnforcesPackedVersionBoundary) {
    const auto entry = EntryT::make(1, 10, EntryMeta<>::MaxVersion);

    EXPECT_EQ(entry.version(), EntryMeta<>::MaxVersion);
    EXPECT_EQ(entry.kind(), EntryKindT::valid);
    EXPECT_THROW(static_cast<void>(EntryT::make(1, 10, EntryMeta<>::MaxVersion + 1)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(EntryT::tombstone(1, EntryMeta<>::MaxVersion + 1)), std::invalid_argument);
}

}  // namespace
}  // namespace cracking_lsm
