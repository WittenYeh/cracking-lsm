// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <cracking-lsm/memtable/memtable.hpp>

namespace cracking_lsm::test {
namespace {

using KeyT = std::uint64_t;

struct ComparatorStats {
    int live = 0;
    int copies = 0;
    int moves = 0;
    int comparisons = 0;
};

/** @brief Observes ownership without exposing Memtable internals or assuming Abseil's copy count. */
struct TrackedComparator {
    explicit TrackedComparator(ComparatorStats& stats) noexcept : stats_(&stats) {
        ++stats_->live;
    }

    TrackedComparator(const TrackedComparator& other) noexcept : stats_(other.stats_) {
        ++stats_->live;
        ++stats_->copies;
    }

    // Deliberately potentially throwing: Memtable must only copy this comparator, even from rvalues.
    TrackedComparator(TrackedComparator&& other) noexcept(false) : stats_(other.stats_) {
        ++stats_->live;
        ++stats_->moves;
    }

    ~TrackedComparator() {
        --stats_->live;
    }

    auto operator()(const KeyT& lhs, const KeyT& rhs) const noexcept -> bool {
        ++stats_->comparisons;
        return lhs < rhs;
    }

private:
    ComparatorStats* stats_;
};

struct NonConstComparator {
    auto operator()(const KeyT& lhs, const KeyT& rhs) noexcept -> bool {
        return lhs < rhs;
    }
};

struct PotentiallyThrowingComparator {
    auto operator()(const KeyT& lhs, const KeyT& rhs) const -> bool {
        return lhs < rhs;
    }
};

struct PotentiallyThrowingBool {
    bool value;

    operator bool() const noexcept(false) {
        return value;
    }
};

struct PotentiallyThrowingBoolComparator {
    auto operator()(const KeyT& lhs, const KeyT& rhs) const noexcept -> PotentiallyThrowingBool {
        return {lhs < rhs};
    }
};

struct PotentiallyThrowingCopyComparator {
    PotentiallyThrowingCopyComparator() = default;
    PotentiallyThrowingCopyComparator(const PotentiallyThrowingCopyComparator&) noexcept(false) {}

    auto operator()(const KeyT& lhs, const KeyT& rhs) const noexcept -> bool {
        return lhs < rhs;
    }
};

template <typename ComparatorT>
concept MemtableAcceptsComparator = requires { typename Memtable<KeyT, ComparatorT>; };

static_assert(MemtableAcceptsComparator<std::less<>>);
static_assert(MemtableAcceptsComparator<TrackedComparator>);
static_assert(!std::is_default_constructible_v<TrackedComparator>);
static_assert(std::is_nothrow_copy_constructible_v<TrackedComparator>);
static_assert(!std::is_nothrow_move_constructible_v<TrackedComparator>);
static_assert(!MemtableAcceptsComparator<NonConstComparator>);
static_assert(!MemtableAcceptsComparator<PotentiallyThrowingComparator>);
static_assert(!MemtableAcceptsComparator<PotentiallyThrowingBoolComparator>);
static_assert(KeyComparator<PotentiallyThrowingCopyComparator, KeyT>);
static_assert(!MemtableAcceptsComparator<PotentiallyThrowingCopyComparator>);

template <typename ModeT>
class MemtableTest : public ::testing::Test {
public:
    using MemtableT = Memtable<KeyT, std::less<>, ModeT::value>;
    using TrackedMemtableT = Memtable<KeyT, TrackedComparator, ModeT::value>;
    using EntryT = typename MemtableT::EntryT;

    static_assert(std::same_as<typename MemtableT::EntryT, KVEntry<KeyT, ModeT::value>>);
    static_assert(!std::is_default_constructible_v<MemtableT>);
    static_assert(!std::is_copy_constructible_v<MemtableT>);
    static_assert(!std::is_copy_assignable_v<MemtableT>);
    static_assert(std::is_nothrow_move_constructible_v<MemtableT>);
    static_assert(std::is_nothrow_move_assignable_v<MemtableT>);
    static_assert(std::is_nothrow_destructible_v<MemtableT>);
    static_assert(std::is_nothrow_move_constructible_v<TrackedMemtableT>);
    static_assert(std::is_nothrow_move_assignable_v<TrackedMemtableT>);
    static_assert(requires(const MemtableT& memtable) {
        { memtable.size() } noexcept -> std::same_as<std::size_t>;
        { memtable.empty() } noexcept -> std::same_as<bool>;
        { memtable.allocated_bytes() } noexcept -> std::same_as<std::size_t>;
        { memtable.memtable_bytes() } noexcept -> std::same_as<std::size_t>;
        { memtable.flush_required() } noexcept -> std::same_as<bool>;
    });
    static_assert(requires(MemtableT& memtable, const EntryT& entry) {
        { memtable.insert(entry) } -> std::same_as<InsertionResult>;
    });
    static_assert(!noexcept(std::declval<MemtableT&>().insert(std::declval<const EntryT&>())));

    static auto make_entry(KeyT key, VersionT version = 1) -> EntryT {
        if constexpr (ModeT::value) {
            return EntryT::make(key, version);
        } else {
            return EntryT::make(key, key, version);
        }
    }
};

using KeyModes = ::testing::Types<std::false_type, std::true_type>;
TYPED_TEST_SUITE(MemtableTest, KeyModes);

template <typename MemtableT>
auto expect_empty_table(const MemtableT& memtable, std::size_t threshold) -> void {
    EXPECT_EQ(memtable.size(), 0);
    EXPECT_TRUE(memtable.empty());
    EXPECT_EQ(memtable.allocated_bytes(), 0);
    EXPECT_EQ(memtable.memtable_bytes(), threshold);
    EXPECT_FALSE(memtable.flush_required());
}

template <typename MemtableT>
auto expect_insertion_result(const InsertionResult& result, const MemtableT& memtable,
    InsertionState status, std::size_t count, bool requires_flush) -> void {
    EXPECT_EQ(result.operation(), OpKind::insertion);
    EXPECT_EQ(result.status, status);
    EXPECT_EQ(result.num_total_entries, count);
    EXPECT_EQ(result.allocated_bytes, memtable.allocated_bytes());
    EXPECT_EQ(result.flush_required, requires_flush);
    EXPECT_EQ(memtable.size(), count);
    EXPECT_EQ(memtable.empty(), count == 0);
    EXPECT_EQ(memtable.flush_required(), requires_flush);
}

TYPED_TEST(MemtableTest, CreatesEmptyTablesForPositiveThresholds) {
    using MemtableT = typename TestFixture::MemtableT;
    const std::array<std::size_t, 3> thresholds = {1, 4096, std::numeric_limits<std::size_t>::max()};

    for (const auto threshold : thresholds) {
        SCOPED_TRACE(threshold);
        MemtableOptions options{.memtable_bytes = threshold};
        const auto memtable = MemtableT::create(options);
        options.memtable_bytes = 0;
        expect_empty_table(memtable, threshold);
    }
}

TYPED_TEST(MemtableTest, RejectsZeroBeforeCopyingOrComparing) {
    using MemtableT = typename TestFixture::MemtableT;
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    EXPECT_THROW(static_cast<void>(MemtableT::create({})), std::invalid_argument);

    ComparatorStats stats;
    {
        const TrackedComparator comparator(stats);
        EXPECT_THROW(static_cast<void>(TrackedMemtableT::create({.memtable_bytes = 0}, comparator)),
            std::invalid_argument);
        EXPECT_EQ(stats.live, 1);
        EXPECT_EQ(stats.copies, 0);
        EXPECT_EQ(stats.moves, 0);
        EXPECT_EQ(stats.comparisons, 0);
    }
    EXPECT_EQ(stats.live, 0);
}

TYPED_TEST(MemtableTest, OwnsComparatorAfterCallerCopyIsDestroyed) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    ComparatorStats stats;
    {
        const auto memtable = [&] {
            const TrackedComparator comparator(stats);
            return TrackedMemtableT::create({.memtable_bytes = 4096}, comparator);
        }();

        expect_empty_table(memtable, 4096);
        EXPECT_GT(stats.live, 0);
        EXPECT_GT(stats.copies, 0);
        EXPECT_EQ(stats.moves, 0);
        EXPECT_EQ(stats.comparisons, 0);
    }
    EXPECT_EQ(stats.live, 0);
}

TYPED_TEST(MemtableTest, MoveConstructionPreservesOwnershipAfterSourceDestruction) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    ComparatorStats stats;
    {
        const TrackedComparator comparator(stats);
        int live_before_move = 0;
        int copies_before_move = 0;
        {
            const auto target = [&] {
                auto source = TrackedMemtableT::create({.memtable_bytes = 4096}, comparator);
                live_before_move = stats.live;
                copies_before_move = stats.copies;
                return TrackedMemtableT{std::move(source)};
            }();

            expect_empty_table(target, 4096);
            EXPECT_EQ(stats.live, live_before_move);
            EXPECT_EQ(stats.copies, copies_before_move);
            EXPECT_EQ(stats.moves, 0);
            EXPECT_EQ(stats.comparisons, 0);
        }
        EXPECT_EQ(stats.live, 1);
    }
    EXPECT_EQ(stats.live, 0);
}

TYPED_TEST(MemtableTest, MoveAssignmentReleasesOldOwnershipAndTransfersConfiguration) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    ComparatorStats source_stats;
    ComparatorStats target_stats;
    {
        const TrackedComparator source_comparator(source_stats);
        const TrackedComparator target_comparator(target_stats);
        auto source = TrackedMemtableT::create({.memtable_bytes = 1024}, source_comparator);
        auto target = TrackedMemtableT::create({.memtable_bytes = 8192}, target_comparator);
        const auto source_live = source_stats.live;
        const auto source_copies = source_stats.copies;
        const auto target_copies = target_stats.copies;

        target = std::move(source);

        expect_empty_table(target, 1024);
        EXPECT_EQ(source_stats.live, source_live);
        EXPECT_EQ(source_stats.copies, source_copies);
        EXPECT_EQ(source_stats.moves, 0);
        EXPECT_EQ(source_stats.comparisons, 0);
        EXPECT_EQ(target_stats.live, 1);
        EXPECT_EQ(target_stats.copies, target_copies);
        EXPECT_EQ(target_stats.moves, 0);
        EXPECT_EQ(target_stats.comparisons, 0);
    }
    EXPECT_EQ(source_stats.live, 0);
    EXPECT_EQ(target_stats.live, 0);
}

TYPED_TEST(MemtableTest, MovedFromObjectCanBeReassignedIndependently) {
    using MemtableT = typename TestFixture::MemtableT;
    auto source = MemtableT::create({.memtable_bytes = 1024});
    auto target = std::move(source);

    source = MemtableT::create({.memtable_bytes = 8192});

    expect_empty_table(source, 8192);
    expect_empty_table(target, 1024);
}

TYPED_TEST(MemtableTest, SelfMoveAssignmentPreservesOwnership) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    ComparatorStats stats;
    {
        const TrackedComparator comparator(stats);
        auto memtable = TrackedMemtableT::create({.memtable_bytes = 4096}, comparator);
        const auto live_before_move = stats.live;
        const auto copies_before_move = stats.copies;
        auto& same_memtable = memtable;

        memtable = std::move(same_memtable);

        expect_empty_table(memtable, 4096);
        EXPECT_EQ(stats.live, live_before_move);
        EXPECT_EQ(stats.copies, copies_before_move);
        EXPECT_EQ(stats.moves, 0);
        EXPECT_EQ(stats.comparisons, 0);
    }
    EXPECT_EQ(stats.live, 0);
}

TYPED_TEST(MemtableTest, InsertCountsDuplicatesVersionsAndTombstones) {
    using MemtableT = typename TestFixture::MemtableT;
    using EntryT = typename TestFixture::EntryT;
    auto memtable = MemtableT::create({.memtable_bytes = std::numeric_limits<std::size_t>::max()});
    const std::array<EntryT, 10> entries = {
        TestFixture::make_entry(20, 5), TestFixture::make_entry(10, 3), TestFixture::make_entry(20, 5),
        EntryT::tombstone(20, 5), TestFixture::make_entry(20, 1), EntryT::tombstone(20, 5),
        TestFixture::make_entry(30, 0), TestFixture::make_entry(0, EntryMeta<TypeParam::value>::MaxVersion),
        EntryT::tombstone(std::numeric_limits<KeyT>::max(), 1), TestFixture::make_entry(20, 9),
    };

    std::size_t count = 0;
    for (const auto& entry : entries) {
        SCOPED_TRACE(count);
        const auto result = memtable.insert(entry);
        expect_insertion_result(result, memtable, InsertionState::inserted, ++count, false);
        EXPECT_GT(memtable.allocated_bytes(), 0);
    }
}

TYPED_TEST(MemtableTest, TinyThresholdAcceptsFirstEntryAndRejectsLaterInserts) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    using EntryT = typename TestFixture::EntryT;
    ComparatorStats stats;
    const TrackedComparator comparator(stats);
    auto memtable = TrackedMemtableT::create({.memtable_bytes = 1}, comparator);
    expect_empty_table(memtable, 1);

    const auto first = TestFixture::make_entry(10, 2);
    const auto accepted = memtable.insert(first);
    expect_insertion_result(accepted, memtable, InsertionState::inserted, 1, true);
    const auto node_bytes = memtable.allocated_bytes();
    ASSERT_GT(node_bytes, memtable.memtable_bytes());
    const auto comparisons = stats.comparisons;
    const std::array<EntryT, 3> rejected = {first, TestFixture::make_entry(20), EntryT::tombstone(10, 3)};

    for (const auto& entry : rejected) {
        const auto result = memtable.insert(entry);
        expect_insertion_result(result, memtable, InsertionState::memtable_full, 1, true);
        EXPECT_EQ(memtable.allocated_bytes(), node_bytes);
        EXPECT_EQ(memtable.memtable_bytes(), 1);
        EXPECT_EQ(stats.comparisons, comparisons);
    }
    EXPECT_EQ(accepted.status, InsertionState::inserted);
    EXPECT_EQ(accepted.num_total_entries, 1);
    EXPECT_EQ(accepted.allocated_bytes, node_bytes);
    EXPECT_TRUE(accepted.flush_required);
}

TYPED_TEST(MemtableTest, NodeGrowthExercisesExactCrossedAndUnreachedThresholds) {
    using MemtableT = typename TestFixture::MemtableT;
    auto reference = MemtableT::create({.memtable_bytes = std::numeric_limits<std::size_t>::max()});
    std::array<std::size_t, 256> node_bytes{};
    const auto entry_at = [](std::size_t index) { return TestFixture::make_entry((index * 37) % 256); };

    for (std::size_t index = 0; index < node_bytes.size(); ++index) {
        const auto result = reference.insert(entry_at(index));
        expect_insertion_result(result, reference, InsertionState::inserted, index + 1, false);
        node_bytes[index] = reference.allocated_bytes();
    }

    // Measure a later growth followed by a free-slot insertion, without assuming Abseil node sizes.
    std::size_t boundary = 0;
    std::size_t previous_peak = 0;
    for (std::size_t index = 1; index + 1 < node_bytes.size(); ++index) {
        if (node_bytes[index - 1] > previous_peak) {
            previous_peak = node_bytes[index - 1];
        }
        if (index >= 32 && node_bytes[index] > previous_peak && node_bytes[index] - previous_peak > 1 &&
            node_bytes[index + 1] == node_bytes[index]) {
            boundary = index;
            break;
        }
    }
    ASSERT_GT(boundary, 0);
    const auto boundary_bytes = node_bytes[boundary];
    const std::array<std::size_t, 3> thresholds = {boundary_bytes - 1, boundary_bytes, boundary_bytes + 1};

    for (const auto threshold : thresholds) {
        SCOPED_TRACE(threshold);
        auto memtable = MemtableT::create({.memtable_bytes = threshold});
        const bool becomes_full = threshold <= boundary_bytes;
        for (std::size_t index = 0; index <= boundary; ++index) {
            SCOPED_TRACE(index);
            const auto result = memtable.insert(entry_at(index));
            expect_insertion_result(result, memtable, InsertionState::inserted, index + 1,
                index == boundary && becomes_full);
            EXPECT_EQ(memtable.allocated_bytes(), node_bytes[index]);
        }

        const auto result = memtable.insert(entry_at(boundary + 1));
        expect_insertion_result(result, memtable,
            becomes_full ? InsertionState::memtable_full : InsertionState::inserted,
            boundary + (becomes_full ? 1 : 2), becomes_full);
        EXPECT_EQ(memtable.allocated_bytes(), boundary_bytes);
    }
}

TYPED_TEST(MemtableTest, InvalidKindsPreserveEmptyWritableAndFullStates) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    const auto large_threshold = std::numeric_limits<std::size_t>::max();
    const std::array<std::pair<std::size_t, std::size_t>, 3> configurations = {{
        {large_threshold, 0}, {large_threshold, 3}, {1, 1},
    }};

    for (const auto& [threshold, initial_count] : configurations) {
        SCOPED_TRACE(threshold);
        SCOPED_TRACE(initial_count);
        ComparatorStats stats;
        const TrackedComparator comparator(stats);
        auto memtable = TrackedMemtableT::create({.memtable_bytes = threshold}, comparator);
        for (std::size_t index = 0; index < initial_count; ++index) {
            ASSERT_EQ(memtable.insert(TestFixture::make_entry(index)).status, InsertionState::inserted);
        }
        const auto node_bytes = memtable.allocated_bytes();
        const bool was_full = memtable.flush_required();
        const auto comparisons = stats.comparisons;
        const auto copies = stats.copies;

        for (const std::uint64_t invalid_kind : {2, 255}) {
            SCOPED_TRACE(invalid_kind);
            auto entry = TestFixture::make_entry(100, 7);
            entry.metadata.version_and_kind &= ~EntryMeta<TypeParam::value>::KindMask;
            entry.metadata.version_and_kind |= invalid_kind;
            EXPECT_THROW(static_cast<void>(memtable.insert(entry)), std::invalid_argument);
            EXPECT_EQ(memtable.size(), initial_count);
            EXPECT_EQ(memtable.empty(), initial_count == 0);
            EXPECT_EQ(memtable.allocated_bytes(), node_bytes);
            EXPECT_EQ(memtable.flush_required(), was_full);
            EXPECT_EQ(memtable.memtable_bytes(), threshold);
            EXPECT_EQ(stats.comparisons, comparisons);
            EXPECT_EQ(stats.copies, copies);
        }

        const auto result = memtable.insert(TestFixture::make_entry(100, 7));
        expect_insertion_result(result, memtable,
            was_full ? InsertionState::memtable_full : InsertionState::inserted,
            initial_count + (was_full ? 0 : 1), was_full);
        if (was_full) {
            EXPECT_EQ(memtable.allocated_bytes(), node_bytes);
        }
    }
}

TYPED_TEST(MemtableTest, MoveConstructionTransfersFullStateAfterSourceDestruction) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    ComparatorStats stats;
    {
        const TrackedComparator comparator(stats);
        std::size_t node_bytes = 0;
        int live_before_move = 0;
        int copies_before_move = 0;
        {
            auto target = [&] {
                auto source = TrackedMemtableT::create({.memtable_bytes = 1}, comparator);
                const auto result = source.insert(TestFixture::make_entry(10));
                expect_insertion_result(result, source, InsertionState::inserted, 1, true);
                node_bytes = source.allocated_bytes();
                live_before_move = stats.live;
                copies_before_move = stats.copies;
                return TrackedMemtableT{std::move(source)};
            }();

            EXPECT_EQ(target.memtable_bytes(), 1);
            EXPECT_EQ(target.allocated_bytes(), node_bytes);
            EXPECT_EQ(stats.live, live_before_move);
            EXPECT_EQ(stats.copies, copies_before_move);
            EXPECT_EQ(stats.moves, 0);
            const auto result = target.insert(TestFixture::make_entry(20));
            expect_insertion_result(result, target, InsertionState::memtable_full, 1, true);
            EXPECT_EQ(target.allocated_bytes(), node_bytes);
        }
        EXPECT_EQ(stats.live, 1);
    }
    EXPECT_EQ(stats.live, 0);
}

TYPED_TEST(MemtableTest, MoveAssignmentTransfersFullStateAndAllowsIndependentSourceReuse) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    const auto large_threshold = std::numeric_limits<std::size_t>::max();
    ComparatorStats source_stats;
    ComparatorStats target_stats;
    {
        const TrackedComparator source_comparator(source_stats);
        const TrackedComparator target_comparator(target_stats);
        auto source = TrackedMemtableT::create({.memtable_bytes = 1}, source_comparator);
        auto target = TrackedMemtableT::create({.memtable_bytes = large_threshold}, target_comparator);
        ASSERT_EQ(source.insert(TestFixture::make_entry(10)).status, InsertionState::inserted);
        for (KeyT key = 0; key < 16; ++key) {
            ASSERT_EQ(target.insert(TestFixture::make_entry(key)).status, InsertionState::inserted);
        }
        ASSERT_TRUE(source.flush_required());
        ASSERT_FALSE(target.flush_required());
        ASSERT_GT(target.allocated_bytes(), 0);
        EXPECT_GT(target_stats.comparisons, 0);
        const auto node_bytes = source.allocated_bytes();
        const auto source_live = source_stats.live;
        const auto source_copies = source_stats.copies;

        target = std::move(source);

        EXPECT_EQ(target.memtable_bytes(), 1);
        EXPECT_EQ(target.allocated_bytes(), node_bytes);
        EXPECT_EQ(source_stats.live, source_live);
        EXPECT_EQ(source_stats.copies, source_copies);
        EXPECT_EQ(source_stats.moves, 0);
        EXPECT_EQ(target_stats.live, 1);
        const auto rejected = target.insert(TestFixture::make_entry(20));
        expect_insertion_result(rejected, target, InsertionState::memtable_full, 1, true);

        source = TrackedMemtableT::create({.memtable_bytes = large_threshold}, source_comparator);
        expect_empty_table(source, large_threshold);
        for (KeyT key = 30; key < 32; ++key) {
            const auto inserted = source.insert(TestFixture::make_entry(key));
            expect_insertion_result(inserted, source, InsertionState::inserted, key - 29, false);
        }
        EXPECT_GT(source.allocated_bytes(), 0);
        EXPECT_EQ(target.size(), 1);
        EXPECT_EQ(target.allocated_bytes(), node_bytes);
        EXPECT_TRUE(target.flush_required());
    }
    EXPECT_EQ(source_stats.live, 0);
    EXPECT_EQ(target_stats.live, 0);
}

TYPED_TEST(MemtableTest, MoveAssignmentMakesFormerlyFullTargetWritable) {
    using MemtableT = typename TestFixture::MemtableT;
    const auto large_threshold = std::numeric_limits<std::size_t>::max();
    auto source = MemtableT::create({.memtable_bytes = large_threshold});
    auto target = MemtableT::create({.memtable_bytes = 1});
    ASSERT_EQ(source.insert(TestFixture::make_entry(10)).status, InsertionState::inserted);
    ASSERT_EQ(source.insert(TestFixture::make_entry(20)).status, InsertionState::inserted);
    ASSERT_EQ(target.insert(TestFixture::make_entry(30)).status, InsertionState::inserted);
    ASSERT_TRUE(target.flush_required());
    const auto node_bytes = source.allocated_bytes();

    target = std::move(source);

    EXPECT_EQ(target.memtable_bytes(), large_threshold);
    EXPECT_EQ(target.size(), 2);
    EXPECT_EQ(target.allocated_bytes(), node_bytes);
    EXPECT_FALSE(target.flush_required());
    const auto result = target.insert(TestFixture::make_entry(40));
    expect_insertion_result(result, target, InsertionState::inserted, 3, false);
}

}  // namespace
}  // namespace cracking_lsm::test
