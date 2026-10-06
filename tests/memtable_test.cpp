// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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

/** @brief Observes ownership without exposing Memtable internals or assuming a fixed copy count. */
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
        { memtable.max_entries() } noexcept -> std::same_as<std::size_t>;
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
    EXPECT_EQ(memtable.max_entries(), threshold);
    EXPECT_FALSE(memtable.flush_required());
}

template <typename MemtableT>
auto expect_insertion_result(const InsertionResult& result, const MemtableT& memtable,
    InsertionState status, std::size_t count, bool requires_flush) -> void {
    EXPECT_EQ(result.operation(), OpKind::insertion);
    EXPECT_EQ(result.status, status);
    EXPECT_EQ(result.num_total_entries, count);
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
        MemtableOptions options{.max_entries = threshold};
        const auto memtable = MemtableT::create(options);
        options.max_entries = 0;
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
        EXPECT_THROW(static_cast<void>(TrackedMemtableT::create({.max_entries = 0}, comparator)),
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
            return TrackedMemtableT::create({.max_entries = 4096}, comparator);
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
                auto source = TrackedMemtableT::create({.max_entries = 4096}, comparator);
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
        auto source = TrackedMemtableT::create({.max_entries = 1024}, source_comparator);
        auto target = TrackedMemtableT::create({.max_entries = 8192}, target_comparator);
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
    auto source = MemtableT::create({.max_entries = 1024});
    auto target = std::move(source);

    source = MemtableT::create({.max_entries = 8192});

    expect_empty_table(source, 8192);
    expect_empty_table(target, 1024);
}

TYPED_TEST(MemtableTest, SelfMoveAssignmentPreservesOwnership) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    ComparatorStats stats;
    {
        const TrackedComparator comparator(stats);
        auto memtable = TrackedMemtableT::create({.max_entries = 4096}, comparator);
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
    const std::array<EntryT, 10> entries = {
        TestFixture::make_entry(20, 5), TestFixture::make_entry(10, 3), TestFixture::make_entry(20, 5),
        EntryT::tombstone(20, 5), TestFixture::make_entry(20, 1), EntryT::tombstone(20, 5),
        TestFixture::make_entry(30, 0), TestFixture::make_entry(0, EntryMeta<TypeParam::value>::MaxVersion),
        EntryT::tombstone(std::numeric_limits<KeyT>::max(), 1), TestFixture::make_entry(20, 9),
    };

    auto memtable = MemtableT::create({.max_entries = entries.size()});
    std::size_t count = 0;
    for (const auto& entry : entries) {
        SCOPED_TRACE(count);
        const auto result = memtable.insert(entry);
        ++count;
        expect_insertion_result(result, memtable, InsertionState::inserted, count, count == entries.size());
    }
}

TYPED_TEST(MemtableTest, TinyThresholdAcceptsFirstEntryAndRejectsLaterInserts) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    using EntryT = typename TestFixture::EntryT;
    ComparatorStats stats;
    const TrackedComparator comparator(stats);
    auto memtable = TrackedMemtableT::create({.max_entries = 1}, comparator);
    expect_empty_table(memtable, 1);

    const auto first = TestFixture::make_entry(10, 2);
    const auto accepted = memtable.insert(first);
    expect_insertion_result(accepted, memtable, InsertionState::inserted, 1, true);
    const auto comparisons = stats.comparisons;
    const std::array<EntryT, 3> rejected = {first, TestFixture::make_entry(20), EntryT::tombstone(10, 3)};

    for (const auto& entry : rejected) {
        const auto result = memtable.insert(entry);
        expect_insertion_result(result, memtable, InsertionState::memtable_full, 1, true);
        EXPECT_EQ(memtable.max_entries(), 1);
        EXPECT_EQ(stats.comparisons, comparisons);
    }
    EXPECT_EQ(accepted.status, InsertionState::inserted);
    EXPECT_EQ(accepted.num_total_entries, 1);
    EXPECT_TRUE(accepted.flush_required);
}

TYPED_TEST(MemtableTest, EntryCountReachesThresholdAndRejectsLaterInsertions) {
    using MemtableT = typename TestFixture::MemtableT;
    const std::array<std::size_t, 3> thresholds = {2, 17, 512};

    for (const auto threshold : thresholds) {
        SCOPED_TRACE(threshold);
        auto memtable = MemtableT::create({.max_entries = threshold});
        for (std::size_t count = 0; count < threshold; ++count) {
            EXPECT_FALSE(memtable.flush_required());
            const auto result = memtable.insert(TestFixture::make_entry(count));
            expect_insertion_result(result, memtable, InsertionState::inserted, count + 1,
                count + 1 == threshold);
        }
        ASSERT_TRUE(memtable.flush_required());
        EXPECT_EQ(memtable.size(), threshold);
        const auto rejected = memtable.insert(TestFixture::make_entry(threshold));
        expect_insertion_result(rejected, memtable, InsertionState::memtable_full, threshold, true);
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
        auto memtable = TrackedMemtableT::create({.max_entries = threshold}, comparator);
        for (std::size_t index = 0; index < initial_count; ++index) {
            ASSERT_EQ(memtable.insert(TestFixture::make_entry(index)).status, InsertionState::inserted);
        }
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
            EXPECT_EQ(memtable.flush_required(), was_full);
            EXPECT_EQ(memtable.max_entries(), threshold);
            EXPECT_EQ(stats.comparisons, comparisons);
            EXPECT_EQ(stats.copies, copies);
        }

        const auto result = memtable.insert(TestFixture::make_entry(100, 7));
        expect_insertion_result(result, memtable,
            was_full ? InsertionState::memtable_full : InsertionState::inserted,
            initial_count + (was_full ? 0 : 1), was_full);
    }
}

TYPED_TEST(MemtableTest, MoveConstructionTransfersFullStateAfterSourceDestruction) {
    using TrackedMemtableT = typename TestFixture::TrackedMemtableT;
    ComparatorStats stats;
    {
        const TrackedComparator comparator(stats);
        int live_before_move = 0;
        int copies_before_move = 0;
        {
            auto target = [&] {
                auto source = TrackedMemtableT::create({.max_entries = 1}, comparator);
                const auto result = source.insert(TestFixture::make_entry(10));
                expect_insertion_result(result, source, InsertionState::inserted, 1, true);
                live_before_move = stats.live;
                copies_before_move = stats.copies;
                return TrackedMemtableT{std::move(source)};
            }();

            EXPECT_EQ(target.max_entries(), 1);
            EXPECT_EQ(stats.live, live_before_move);
            EXPECT_EQ(stats.copies, copies_before_move);
            EXPECT_EQ(stats.moves, 0);
            const auto result = target.insert(TestFixture::make_entry(20));
            expect_insertion_result(result, target, InsertionState::memtable_full, 1, true);
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
        auto source = TrackedMemtableT::create({.max_entries = 1}, source_comparator);
        auto target = TrackedMemtableT::create({.max_entries = large_threshold}, target_comparator);
        ASSERT_EQ(source.insert(TestFixture::make_entry(10)).status, InsertionState::inserted);
        for (KeyT key = 0; key < 16; ++key) {
            ASSERT_EQ(target.insert(TestFixture::make_entry(key)).status, InsertionState::inserted);
        }
        ASSERT_TRUE(source.flush_required());
        ASSERT_FALSE(target.flush_required());
        EXPECT_GT(target_stats.comparisons, 0);
        const auto source_live = source_stats.live;
        const auto source_copies = source_stats.copies;

        target = std::move(source);

        EXPECT_EQ(target.max_entries(), 1);
        EXPECT_EQ(source_stats.live, source_live);
        EXPECT_EQ(source_stats.copies, source_copies);
        EXPECT_EQ(source_stats.moves, 0);
        EXPECT_EQ(target_stats.live, 1);
        const auto rejected = target.insert(TestFixture::make_entry(20));
        expect_insertion_result(rejected, target, InsertionState::memtable_full, 1, true);

        source = TrackedMemtableT::create({.max_entries = large_threshold}, source_comparator);
        expect_empty_table(source, large_threshold);
        for (KeyT key = 30; key < 32; ++key) {
            const auto inserted = source.insert(TestFixture::make_entry(key));
            expect_insertion_result(inserted, source, InsertionState::inserted, key - 29, false);
        }
        EXPECT_EQ(target.size(), 1);
        EXPECT_TRUE(target.flush_required());
    }
    EXPECT_EQ(source_stats.live, 0);
    EXPECT_EQ(target_stats.live, 0);
}

TYPED_TEST(MemtableTest, MoveAssignmentMakesFormerlyFullTargetWritable) {
    using MemtableT = typename TestFixture::MemtableT;
    const auto large_threshold = std::numeric_limits<std::size_t>::max();
    auto source = MemtableT::create({.max_entries = large_threshold});
    auto target = MemtableT::create({.max_entries = 1});
    ASSERT_EQ(source.insert(TestFixture::make_entry(10)).status, InsertionState::inserted);
    ASSERT_EQ(source.insert(TestFixture::make_entry(20)).status, InsertionState::inserted);
    ASSERT_EQ(target.insert(TestFixture::make_entry(30)).status, InsertionState::inserted);
    ASSERT_TRUE(target.flush_required());

    target = std::move(source);

    EXPECT_EQ(target.max_entries(), large_threshold);
    EXPECT_EQ(target.size(), 2);
    EXPECT_FALSE(target.flush_required());
    const auto result = target.insert(TestFixture::make_entry(40));
    expect_insertion_result(result, target, InsertionState::inserted, 3, false);
}

/** @brief Stateful user order, including equivalence between different physical key values. */
struct QueryComparator {
    QueryComparator(bool descending_value, KeyT width_value) noexcept
        : descending(descending_value), width(width_value) {}

    auto operator()(KeyT lhs, KeyT rhs) const noexcept -> bool {
        return descending ? lhs / width > rhs / width : lhs / width < rhs / width;
    }

    bool descending;
    KeyT width;
};

/** @brief Resolves an unsorted reference snapshot by linear scans, without the index comparator. */
template <typename EntryT, typename ComparatorT>
auto visible_reference(std::span<const EntryT> entries, VersionT read_version,
    const ComparatorT& comparator) -> std::vector<EntryT> {
    std::vector<EntryT> visible;
    for (const auto& entry : entries) {
        if (entry.version() > read_version) {
            continue;
        }
        auto existing = std::find_if(visible.begin(), visible.end(), [&](const EntryT& candidate) {
            return !comparator(candidate.key, entry.key) && !comparator(entry.key, candidate.key);
        });
        if (existing == visible.end()) {
            visible.push_back(entry);
        } else if (entry.version() > existing->version() ||
            (entry.version() == existing->version() && entry.kind() == EntryKindT::tombstone)) {
            *existing = entry;
        }
    }
    return visible;
}

template <typename ResultT, typename EntryT, typename ComparatorT>
auto matches_query(const ResultT& result, const std::optional<EntryT>& expected,
    const ComparatorT& comparator) -> bool {
    const auto expected_state = !expected ? QueryState::not_found
        : expected->kind() == EntryKindT::tombstone ? QueryState::tombstone : QueryState::value;
    if (result.state() != expected_state || result.entry().has_value() != expected.has_value()) {
        return false;
    }
    if (!expected) {
        return true;
    }
    const auto& actual = *result.entry();
    if (comparator(actual.key, expected->key) || comparator(expected->key, actual.key) ||
        actual.version() != expected->version() || actual.kind() != expected->kind()) {
        return false;
    }
    if constexpr (std::same_as<decltype(actual.metadata.payload_ref), PayloadRefT>) {
        if (actual.kind() == EntryKindT::valid) {
            return actual.metadata.payload_ref == expected->metadata.payload_ref;
        }
    }
    return true;
}

template <typename ResultT, typename EntryT>
auto expect_query(const ResultT& result, const EntryT& expected) -> void {
    EXPECT_TRUE(matches_query(result, std::optional<EntryT>{expected}, std::less<>{}));
}

template <typename MemtableT, typename ComparatorT>
auto expect_reference_queries(const MemtableT& memtable,
    std::span<const typename MemtableT::EntryT> entries, const ComparatorT& comparator) -> void {
    using EntryT = typename MemtableT::EntryT;
    const auto count = memtable.size();
    const auto full = memtable.flush_required();
    const std::array<VersionT, 7> versions = {0, 1, 3, 7, 15, 16, EntryMeta<>::MaxVersion};
    for (const auto read_version : versions) {
        SCOPED_TRACE(read_version);
        const auto visible = visible_reference(entries, read_version, comparator);
        for (KeyT key = 0; key <= 65; ++key) {
            SCOPED_TRACE(key);
            std::optional<EntryT> lookup;
            std::optional<EntryT> successor;
            for (const auto& entry : visible) {
                if (!comparator(entry.key, key) && !comparator(key, entry.key)) {
                    lookup = entry;
                }
                if (entry.kind() == EntryKindT::valid && comparator(key, entry.key) &&
                    (!successor || comparator(entry.key, successor->key))) {
                    successor = entry;
                }
            }
            const auto lookup_result = memtable.lookup(key, read_version);
            const auto successor_result = memtable.successor(key, read_version);
            EXPECT_EQ(lookup_result.operation(), OpKind::lookup);
            EXPECT_EQ(successor_result.operation(), OpKind::successor);
            ASSERT_TRUE(matches_query(lookup_result, lookup, comparator));
            ASSERT_TRUE(matches_query(successor_result, successor, comparator));
        }
    }
    EXPECT_EQ(memtable.size(), count);
    EXPECT_EQ(memtable.flush_required(), full);
}

TYPED_TEST(MemtableTest, QueriesHandleEmptyTablesAndStrictKeyBoundaries) {
    using MemtableT = typename TestFixture::MemtableT;
    auto memtable = MemtableT::create({.max_entries = std::numeric_limits<std::size_t>::max()});
    const auto largest_key = std::numeric_limits<KeyT>::max();
    for (const auto key : {KeyT{0}, KeyT{10}, largest_key}) {
        EXPECT_EQ(memtable.lookup(key).state(), QueryState::not_found);
        EXPECT_FALSE(memtable.lookup(key).entry());
        EXPECT_EQ(memtable.successor(key).state(), QueryState::not_found);
        EXPECT_FALSE(memtable.successor(key).entry());
    }
    for (const auto key : {largest_key, KeyT{0}, KeyT{20}, KeyT{10}}) {
        ASSERT_EQ(memtable.insert(TestFixture::make_entry(key, 0)).status, InsertionState::inserted);
    }
    EXPECT_EQ(memtable.successor(largest_key).state(), QueryState::not_found);
    expect_query(memtable.successor(0), TestFixture::make_entry(10, 0));
    expect_query(memtable.successor(10), TestFixture::make_entry(20, 0));
    expect_query(memtable.successor(11), TestFixture::make_entry(20, 0));
    expect_query(memtable.successor(20), TestFixture::make_entry(largest_key, 0));
    expect_query(memtable.lookup(largest_key, 0), TestFixture::make_entry(largest_key, 0));
    EXPECT_EQ(memtable.lookup(11).state(), QueryState::not_found);
}

TYPED_TEST(MemtableTest, LookupSelectsVisibleVersionsAndTombstonesAtEqualVersions) {
    using MemtableT = typename TestFixture::MemtableT;
    using EntryT = typename TestFixture::EntryT;
    auto memtable = MemtableT::create({.max_entries = std::numeric_limits<std::size_t>::max()});
    const auto latest = EntryMeta<TypeParam::value>::MaxVersion;
    const std::array<EntryT, 8> entries = {TestFixture::make_entry(20, 9),
        TestFixture::make_entry(20, 3), EntryT::tombstone(20, 7), TestFixture::make_entry(20, 7),
        TestFixture::make_entry(20, 0), TestFixture::make_entry(20, 3),
        EntryT::tombstone(20, latest), TestFixture::make_entry(20, latest)};
    for (const auto& entry : entries) {
        ASSERT_EQ(memtable.insert(entry).status, InsertionState::inserted);
    }
    expect_query(memtable.lookup(20, 0), TestFixture::make_entry(20, 0));
    expect_query(memtable.lookup(20, 2), TestFixture::make_entry(20, 0));
    expect_query(memtable.lookup(20, 3), TestFixture::make_entry(20, 3));
    expect_query(memtable.lookup(20, 6), TestFixture::make_entry(20, 3));
    expect_query(memtable.lookup(20, 7), EntryT::tombstone(20, 7));
    expect_query(memtable.lookup(20, 8), EntryT::tombstone(20, 7));
    expect_query(memtable.lookup(20, 9), TestFixture::make_entry(20, 9));
    expect_query(memtable.lookup(20), EntryT::tombstone(20, latest));
    EXPECT_EQ(memtable.size(), entries.size());
}

TYPED_TEST(MemtableTest, SuccessorSkipsDeletedAndInvisibleGroupsWithoutExposingOlderValues) {
    using MemtableT = typename TestFixture::MemtableT;
    using EntryT = typename TestFixture::EntryT;
    auto memtable = MemtableT::create({.max_entries = std::numeric_limits<std::size_t>::max()});
    const std::array<EntryT, 12> entries = {TestFixture::make_entry(10, 9),
        TestFixture::make_entry(40, 2), EntryT::tombstone(20, 8), TestFixture::make_entry(30, 6),
        TestFixture::make_entry(50, 1), TestFixture::make_entry(20, 8), EntryT::tombstone(40, 5),
        TestFixture::make_entry(20, 3), EntryT::tombstone(30, 7), EntryT::tombstone(20, 8),
        TestFixture::make_entry(20, 3), EntryT::tombstone(40, 5)};
    for (const auto& entry : entries) {
        ASSERT_EQ(memtable.insert(entry).status, InsertionState::inserted);
    }
    expect_query(memtable.successor(0, 8), TestFixture::make_entry(50, 1));
    expect_query(memtable.successor(0, 7), TestFixture::make_entry(20, 3));
    expect_query(memtable.successor(20, 6), TestFixture::make_entry(30, 6));
    expect_query(memtable.successor(20, 5), TestFixture::make_entry(50, 1));
    expect_query(memtable.successor(30, 4), TestFixture::make_entry(40, 2));
    expect_query(memtable.successor(30, 5), TestFixture::make_entry(50, 1));
    EXPECT_EQ(memtable.successor(0, 0).state(), QueryState::not_found);
    ASSERT_EQ(memtable.insert(EntryT::tombstone(50, 10)).status, InsertionState::inserted);
    EXPECT_EQ(memtable.successor(10).state(), QueryState::not_found);
    ASSERT_EQ(memtable.insert(TestFixture::make_entry(20, 12)).status, InsertionState::inserted);
    expect_query(memtable.successor(10), TestFixture::make_entry(20, 12));
}

TYPED_TEST(MemtableTest, QueriesRejectInvalidVersionsBeforeSearchingOrChangingState) {
    using MemtableT = typename TestFixture::TrackedMemtableT;
    for (const std::size_t threshold : {std::size_t{1}, std::numeric_limits<std::size_t>::max()}) {
        ComparatorStats stats;
        auto memtable = MemtableT::create({.max_entries = threshold}, TrackedComparator{stats});
        for (const bool populated : {false, true}) {
            if (populated) {
                ASSERT_EQ(memtable.insert(TestFixture::make_entry(10)).status, InsertionState::inserted);
            }
            const auto comparisons = stats.comparisons;
            const auto count = memtable.size();
            const auto full = memtable.flush_required();
            for (const auto invalid : {EntryMeta<TypeParam::value>::MaxVersion + 1,
                     std::numeric_limits<VersionT>::max()}) {
                EXPECT_THROW(static_cast<void>(memtable.lookup(10, invalid)), std::invalid_argument);
                EXPECT_THROW(static_cast<void>(memtable.successor(20, invalid)), std::invalid_argument);
            }
            EXPECT_EQ(stats.comparisons, comparisons);
            EXPECT_EQ(memtable.size(), count);
            EXPECT_EQ(memtable.flush_required(), full);
        }
    }
}

TYPED_TEST(MemtableTest, FullTableQueriesOwnTheirResultsAcrossMoveAndDestruction) {
    using MemtableT = typename TestFixture::MemtableT;
    const auto expected = TestFixture::make_entry(10, 5);
    const auto results = [&] {
        auto memtable = MemtableT::create({.max_entries = 1});
        EXPECT_EQ(memtable.insert(expected).status, InsertionState::inserted);
        const auto lookup = memtable.lookup(10);
        const auto successor = memtable.successor(0);
        auto moved = std::move(memtable);
        EXPECT_EQ(moved.insert(TestFixture::make_entry(15, 6)).status, InsertionState::memtable_full);
        expect_query(moved.lookup(10), expected);
        expect_query(moved.successor(0), expected);
        EXPECT_EQ(moved.size(), 1);
        EXPECT_TRUE(moved.flush_required());
        return std::pair{lookup, successor};
    }();
    expect_query(results.first, expected);
    expect_query(results.second, expected);
}

TYPED_TEST(MemtableTest, SuccessorQueriesMatchAnUnsortedReferenceAcrossOrdersAndEquivalentKeys) {
    using MemtableT = Memtable<KeyT, QueryComparator, TypeParam::value>;
    using EntryT = typename TestFixture::EntryT;
    for (const auto comparator : {QueryComparator{false, 1}, QueryComparator{true, 1},
             QueryComparator{false, 4}, QueryComparator{true, 4}}) {
        SCOPED_TRACE(comparator.descending);
        SCOPED_TRACE(comparator.width);
        for (const std::uint64_t seed : {7, 37, 83}) {
            SCOPED_TRACE(seed);
            std::mt19937_64 random(seed);
            auto memtable = MemtableT::create(
                {.max_entries = std::numeric_limits<std::size_t>::max()}, comparator);
            std::vector<EntryT> entries;
            for (std::size_t index = 0; index < 256; ++index) {
                const auto key = random() % 64;
                const auto version = random() % 16;
                auto entry = random() % 3 == 0 ? EntryT::tombstone(key, version)
                                             : TestFixture::make_entry(key, version);
                if constexpr (!TypeParam::value) {
                    // Comparator-equivalent InternalKeys must refer to the same logical payload.
                    entry.metadata.payload_ref = (key / comparator.width) * 16 + version;
                }
                entries.push_back(entry);
                if (index % 8 == 0) {
                    entries.push_back(entry);
                }
            }
            std::shuffle(entries.begin(), entries.end(), random);
            for (std::size_t index = 0; index < entries.size(); ++index) {
                ASSERT_EQ(memtable.insert(entries[index]).status, InsertionState::inserted);
                if ((index + 1) % 64 == 0 || index + 1 == entries.size()) {
                    const std::span<const EntryT> inserted(entries.data(), index + 1);
                    ASSERT_NO_FATAL_FAILURE(expect_reference_queries(memtable, inserted, comparator));
                }
            }
            EXPECT_EQ(memtable.size(), entries.size());
        }
    }
}

TYPED_TEST(MemtableTest, SuccessorQueriesRemainCorrectDuringConcurrentNewerVersionInsertions) {
    using MemtableT = typename TestFixture::MemtableT;
    using EntryT = typename TestFixture::EntryT;
    auto memtable = MemtableT::create({.max_entries = std::numeric_limits<std::size_t>::max()});
    std::vector<EntryT> entries;
    for (KeyT key = 0; key < 64; ++key) {
        const auto entry = key % 4 == 0 ? EntryT::tombstone(key, 0) : TestFixture::make_entry(key, 0);
        ASSERT_EQ(memtable.insert(entry).status, InsertionState::inserted);
        entries.push_back(entry);
    }
    const auto baseline = entries;
    constexpr std::size_t writer_count = 4;
    constexpr std::size_t reader_count = 2;
    constexpr std::size_t writes_per_thread = 256;
    for (std::size_t index = 0; index < writer_count * writes_per_thread; ++index) {
        const auto key = (index * 17) % 64;
        const auto version = 1 + (index / 64) % 15;
        entries.push_back(index % 3 == 0 ? EntryT::tombstone(key, version)
                                       : TestFixture::make_entry(key, version));
    }
    std::atomic<std::size_t> failures{0};
    std::barrier start{static_cast<std::ptrdiff_t>(writer_count + reader_count)};
    std::vector<std::jthread> threads;
    for (std::size_t writer = 0; writer < writer_count; ++writer) {
        threads.emplace_back([&, writer] {
            start.arrive_and_wait();
            for (std::size_t index = 0; index < writes_per_thread; ++index) {
                const auto& entry = entries[baseline.size() + writer * writes_per_thread + index];
                if (memtable.insert(entry).status != InsertionState::inserted) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::size_t reader = 0; reader < reader_count; ++reader) {
        threads.emplace_back([&, reader] {
            start.arrive_and_wait();
            for (std::size_t index = 0; index < 2048; ++index) {
                const KeyT key = (index + reader) % 66;
                const std::optional<EntryT> lookup = key < baseline.size()
                    ? std::optional<EntryT>{baseline[key]} : std::nullopt;
                std::optional<EntryT> successor;
                for (const auto& entry : baseline) {
                    if (entry.key > key && entry.kind() == EntryKindT::valid &&
                        (!successor || entry.key < successor->key)) {
                        successor = entry;
                    }
                }
                // Writers publish only versions > 0, so this already-published view stays unchanged.
                if (!matches_query(memtable.lookup(key, 0), lookup, std::less<>{}) ||
                    !matches_query(memtable.successor(key, 0), successor, std::less<>{})) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                const auto current = memtable.successor(key);
                if (current.entry() && (current.entry()->key <= key ||
                    current.entry()->kind() != EntryKindT::valid)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    threads.clear();  // Join all operations before checking the final, stable view or destroying the table.
    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(memtable.size(), entries.size());
    EXPECT_FALSE(memtable.flush_required());
    expect_reference_queries(memtable, std::span<const EntryT>{entries}, std::less<>{});
}

}  // namespace
}  // namespace cracking_lsm::test
