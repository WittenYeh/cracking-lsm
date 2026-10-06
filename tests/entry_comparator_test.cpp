// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>

#include <gtest/gtest.h>

#include <cracking-lsm/kv_entry/entry_comparator.hpp>

namespace cracking_lsm::test {
namespace {

using KeyT = std::uint64_t;

struct DescendingDecades {
    auto operator()(KeyT lhs, KeyT rhs) const noexcept -> bool {
        return lhs / 10 > rhs / 10;
    }
};

template <typename ModeT>
class EntryComparatorTest : public ::testing::Test {
public:
    using EntryT = KVEntry<KeyT, ModeT::value>;
    using ComparatorT = EntryComparator<KeyT, std::less<>, ModeT::value>;

    static auto make_entry(KeyT key, VersionT version) -> EntryT {
        if constexpr (ModeT::value) {
            return EntryT::make(key, version);
        } else {
            return EntryT::make(key, key, version);
        }
    }
};

using KeyModes = ::testing::Types<std::false_type, std::true_type>;
TYPED_TEST_SUITE(EntryComparatorTest, KeyModes);

TYPED_TEST(EntryComparatorTest, PreservesUserKeyOrderWithVersionsAndKindsDescending) {
    using EntryT = typename TestFixture::EntryT;
    const typename TestFixture::ComparatorT comparator;
    const std::array<EntryT, 6> expected = {TestFixture::make_entry(10, 5), TestFixture::make_entry(10, 0),
        EntryT::tombstone(20, 9), TestFixture::make_entry(20, 9), TestFixture::make_entry(20, 3),
        TestFixture::make_entry(30, 2)};
    auto entries = expected;
    std::reverse(entries.begin(), entries.end());
    std::sort(entries.begin(), entries.end(), comparator);

    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(entries[index].key, expected[index].key);
        EXPECT_EQ(entries[index].version(), expected[index].version());
        EXPECT_EQ(entries[index].kind(), expected[index].kind());
        for (std::size_t other = 0; other < expected.size(); ++other) {
            EXPECT_EQ(comparator(expected[index], expected[other]), index < other);
        }
    }
    auto duplicate = expected[3];
    if constexpr (!TypeParam::value) {
        duplicate.metadata.payload_ref = 999;
    }
    EXPECT_FALSE(comparator(duplicate, expected[3]));
    EXPECT_FALSE(comparator(expected[3], duplicate));
}

TYPED_TEST(EntryComparatorTest, HeterogeneousProbesIgnoreVersionAndKind) {
    using EntryT = typename TestFixture::EntryT;
    const typename TestFixture::ComparatorT comparator;
    const std::array<EntryT, 4> entries = {TestFixture::make_entry(20, 0),
        TestFixture::make_entry(20, EntryMeta<TypeParam::value>::MaxVersion),
        EntryT::tombstone(20, 0), EntryT::tombstone(20, EntryMeta<TypeParam::value>::MaxVersion)};
    const std::array<KeyT, 5> probes = {0, 19, 20, 21, std::numeric_limits<KeyT>::max()};

    for (const auto& entry : entries) {
        for (const auto probe : probes) {
            SCOPED_TRACE(probe);
            EXPECT_EQ(comparator(entry, probe), probe > 20);
            EXPECT_EQ(comparator(probe, entry), probe < 20);
            EXPECT_EQ(comparator.keys_equal(entry.key, probe), probe == 20);
        }
    }
}

TYPED_TEST(EntryComparatorTest, PreservesCustomOrderAndEquivalentKeyGroups) {
    using EntryT = typename TestFixture::EntryT;
    const EntryComparator<KeyT, DescendingDecades, TypeParam::value> comparator;
    // User order is 30s, 20s, 10s; versions and kinds remain descending within each group.
    const std::array<EntryT, 5> expected = {TestFixture::make_entry(33, 9), EntryT::tombstone(21, 5),
        TestFixture::make_entry(29, 5), TestFixture::make_entry(25, 1), TestFixture::make_entry(12, 0)};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        for (std::size_t other = 0; other < expected.size(); ++other) {
            EXPECT_EQ(comparator(expected[index], expected[other]), index < other);
        }
    }
    EXPECT_TRUE(comparator.keys_equal(21, 29));
    EXPECT_FALSE(comparator.keys_equal(19, 20));
    for (const auto probe : {KeyT{20}, KeyT{24}, KeyT{29}}) {
        EXPECT_FALSE(comparator(expected[1], probe));
        EXPECT_FALSE(comparator(probe, expected[1]));
        EXPECT_TRUE(comparator(expected[0], probe));
        EXPECT_TRUE(comparator(probe, expected[4]));
    }
}

}  // namespace
}  // namespace cracking_lsm::test
