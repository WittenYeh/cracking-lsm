// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <cracking-lsm/memtable/memory_accounting.hpp>

namespace cracking_lsm::test {
namespace {

using AllocatorT = CountingAllocator<std::uint64_t>;
using TraitsT = std::allocator_traits<AllocatorT>;
using VectorT = std::vector<std::uint64_t, AllocatorT>;

struct alignas(128) AlignedEntry {
    std::uint64_t value = 0;
};

static_assert(std::is_same_v<TraitsT::value_type, std::uint64_t>);
static_assert(std::is_same_v<TraitsT::pointer, std::uint64_t*>);
static_assert(std::is_same_v<TraitsT::size_type, AllocatorT::size_t>);
static_assert(std::is_same_v<TraitsT::difference_type, AllocatorT::difference_t>);
static_assert(std::is_same_v<TraitsT::rebind_alloc<AlignedEntry>, CountingAllocator<AlignedEntry>>);
static_assert(!TraitsT::propagate_on_container_copy_assignment::value);
static_assert(TraitsT::propagate_on_container_move_assignment::value);
static_assert(TraitsT::propagate_on_container_swap::value);
static_assert(!TraitsT::is_always_equal::value);
static_assert(!std::is_default_constructible_v<AllocatorT>);
static_assert(std::is_nothrow_constructible_v<AllocatorT, MemoryAccounting&>);
static_assert(std::is_nothrow_copy_constructible_v<AllocatorT>);
static_assert(std::is_nothrow_move_constructible_v<AllocatorT>);
static_assert(std::is_nothrow_copy_assignable_v<AllocatorT>);
static_assert(std::is_nothrow_move_assignable_v<AllocatorT>);
static_assert(std::is_nothrow_constructible_v<CountingAllocator<AlignedEntry>, const AllocatorT&>);
static_assert(std::is_nothrow_default_constructible_v<MemoryAccounting>);
static_assert(!std::is_copy_constructible_v<MemoryAccounting>);
static_assert(!std::is_move_constructible_v<MemoryAccounting>);
static_assert(!std::is_copy_assignable_v<MemoryAccounting>);
static_assert(!std::is_move_assignable_v<MemoryAccounting>);
static_assert(noexcept(std::declval<AllocatorT&>().allocate(1)));
static_assert(noexcept(std::declval<AllocatorT&>().deallocate(nullptr, 1)));

/** @brief Matches all diagnostic fields without depending on the terminate handler's own message. */
auto diagnostic_pattern(const char* operation, const char* reason, std::size_t count,
    std::size_t element_bytes, std::size_t allocated_bytes) -> std::string {
    return std::string{"cracking-lsm: CountingAllocator::"} + operation + ": " + reason +
        " \\(count=" + std::to_string(count) + ", element_bytes=" + std::to_string(element_bytes) +
        ", allocated_bytes=" + std::to_string(allocated_bytes) + "\\)";
}

TEST(MemoryAccountingTest, StartsEmptyWithoutAllocatingForAllocatorObjects) {
    MemoryAccounting accounting;
    EXPECT_EQ(accounting.allocated_bytes(), 0);

    {
        const AllocatorT allocator(accounting);
        const AllocatorT copy(allocator);
        EXPECT_EQ(allocator, copy);
        EXPECT_EQ(accounting.allocated_bytes(), 0);
    }

    EXPECT_EQ(accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, TracksLiveRequestBytesAndReturnsToZero) {
    MemoryAccounting accounting;
    AllocatorT allocator(accounting);

    auto* first = TraitsT::allocate(allocator, 3);
    EXPECT_EQ(accounting.allocated_bytes(), 3 * sizeof(std::uint64_t));
    auto* second = TraitsT::allocate(allocator, 5);
    EXPECT_EQ(accounting.allocated_bytes(), 8 * sizeof(std::uint64_t));

    TraitsT::construct(allocator, first, 42);
    TraitsT::construct(allocator, second + 4, 99);
    EXPECT_EQ(*first, 42);
    EXPECT_EQ(second[4], 99);
    TraitsT::destroy(allocator, first);
    TraitsT::destroy(allocator, second + 4);

    TraitsT::deallocate(allocator, first, 3);
    EXPECT_EQ(accounting.allocated_bytes(), 5 * sizeof(std::uint64_t));
    TraitsT::deallocate(allocator, second, 5);
    EXPECT_EQ(accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, ZeroLengthRequestPreservesExistingLiveBytes) {
    MemoryAccounting accounting;
    AllocatorT allocator(accounting);
    auto* live = allocator.allocate(2);

    auto* empty = allocator.allocate(0);
    EXPECT_EQ(accounting.allocated_bytes(), 2 * sizeof(std::uint64_t));
    allocator.deallocate(empty, 0);
    EXPECT_EQ(accounting.allocated_bytes(), 2 * sizeof(std::uint64_t));

    allocator.deallocate(live, 2);
    EXPECT_EQ(accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, CopiesAndMovesShareTheDomainAndLeaveTheSourceUsable) {
    MemoryAccounting accounting;
    AllocatorT original(accounting);
    auto* first = original.allocate(3);

    {
        AllocatorT copy(original);
        AllocatorT moved(std::move(copy));
        EXPECT_EQ(original, copy);
        EXPECT_EQ(original, moved);
        EXPECT_EQ(accounting.allocated_bytes(), 3 * sizeof(std::uint64_t));

        auto* second = copy.allocate(2);
        EXPECT_EQ(accounting.allocated_bytes(), 5 * sizeof(std::uint64_t));
        moved.deallocate(first, 3);
        EXPECT_EQ(accounting.allocated_bytes(), 2 * sizeof(std::uint64_t));
        original.deallocate(second, 2);
    }

    EXPECT_EQ(accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, CopyAssignmentChangesTheAllocatorDomainWithoutChangingCounters) {
    MemoryAccounting first_accounting;
    MemoryAccounting second_accounting;
    AllocatorT source(first_accounting);
    AllocatorT destination(second_accounting);
    AllocatorT second_owner(destination);
    auto* first = source.allocate(2);
    auto* second = destination.allocate(3);

    destination = source;
    EXPECT_EQ(destination, source);
    EXPECT_NE(destination, second_owner);
    EXPECT_EQ(first_accounting.allocated_bytes(), 2 * sizeof(std::uint64_t));
    EXPECT_EQ(second_accounting.allocated_bytes(), 3 * sizeof(std::uint64_t));

    destination.deallocate(first, 2);
    second_owner.deallocate(second, 3);
    EXPECT_EQ(first_accounting.allocated_bytes(), 0);
    EXPECT_EQ(second_accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, MoveAssignmentKeepsBothAllocatorsUsableInTheSourceDomain) {
    MemoryAccounting first_accounting;
    MemoryAccounting second_accounting;
    AllocatorT source(first_accounting);
    AllocatorT destination(second_accounting);
    AllocatorT second_owner(destination);
    auto* first = source.allocate(2);
    auto* second = destination.allocate(3);

    destination = std::move(source);
    EXPECT_EQ(destination, source);
    EXPECT_NE(destination, second_owner);
    EXPECT_EQ(first_accounting.allocated_bytes(), 2 * sizeof(std::uint64_t));
    EXPECT_EQ(second_accounting.allocated_bytes(), 3 * sizeof(std::uint64_t));

    auto* third = source.allocate(1);
    EXPECT_EQ(first_accounting.allocated_bytes(), 3 * sizeof(std::uint64_t));
    destination.deallocate(first, 2);
    destination.deallocate(third, 1);
    second_owner.deallocate(second, 3);
    EXPECT_EQ(first_accounting.allocated_bytes(), 0);
    EXPECT_EQ(second_accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, RebindSharesTheCounterAndPreservesExtendedAlignment) {
    MemoryAccounting accounting;
    AllocatorT original(accounting);
    TraitsT::rebind_alloc<AlignedEntry> rebound(original);
    EXPECT_EQ(original, rebound);
    EXPECT_EQ(rebound, original);

    auto* scalar = original.allocate(1);
    auto* entries = rebound.allocate(3);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(entries) % alignof(AlignedEntry), 0);
    EXPECT_EQ(accounting.allocated_bytes(), sizeof(std::uint64_t) + 3 * sizeof(AlignedEntry));

    std::construct_at(entries + 2);
    entries[2].value = 17;
    EXPECT_EQ(entries[2].value, 17);
    std::destroy_at(entries + 2);

    rebound.deallocate(entries, 3);
    EXPECT_EQ(accounting.allocated_bytes(), sizeof(std::uint64_t));
    original.deallocate(scalar, 1);
    EXPECT_EQ(accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, IndependentDomainsDoNotShareBytesOrCompareEqual) {
    MemoryAccounting first_accounting;
    MemoryAccounting second_accounting;
    AllocatorT first(first_accounting);
    AllocatorT second(second_accounting);
    CountingAllocator<std::byte> second_bytes(second_accounting);
    EXPECT_NE(first, second);
    EXPECT_NE(first, second_bytes);
    EXPECT_NE(second_bytes, first);

    auto* first_allocation = first.allocate(2);
    EXPECT_EQ(second_accounting.allocated_bytes(), 0);
    auto* second_allocation = second.allocate(3);
    EXPECT_EQ(first_accounting.allocated_bytes(), 2 * sizeof(std::uint64_t));
    EXPECT_EQ(second_accounting.allocated_bytes(), 3 * sizeof(std::uint64_t));

    first.deallocate(first_allocation, 2);
    EXPECT_EQ(first_accounting.allocated_bytes(), 0);
    EXPECT_EQ(second_accounting.allocated_bytes(), 3 * sizeof(std::uint64_t));
    second.deallocate(second_allocation, 3);
    EXPECT_EQ(second_accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, StandardContainerCountsCapacityAndReleasesStorageOnDestruction) {
    MemoryAccounting accounting;

    {
        VectorT values{AllocatorT{accounting}};
        values.reserve(4);
        values.resize(4, 7);
        EXPECT_EQ(accounting.allocated_bytes(), values.capacity() * sizeof(std::uint64_t));

        values.reserve(33);
        EXPECT_EQ(values.front(), 7);
        EXPECT_EQ(values.back(), 7);
        EXPECT_EQ(accounting.allocated_bytes(), values.capacity() * sizeof(std::uint64_t));

        const auto reserved_bytes = accounting.allocated_bytes();
        values.clear();
        EXPECT_TRUE(values.empty());
        EXPECT_GT(reserved_bytes, 0);
        EXPECT_EQ(accounting.allocated_bytes(), reserved_bytes);
    }

    EXPECT_EQ(accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, ContainerCopyAssignmentKeepsTheDestinationDomain) {
    MemoryAccounting source_accounting;
    MemoryAccounting destination_accounting;

    {
        const VectorT source({1, 2, 3}, AllocatorT{source_accounting});
        VectorT destination({9}, AllocatorT{destination_accounting});

        destination = source;
        EXPECT_EQ(destination, source);
        EXPECT_EQ(destination.get_allocator(), AllocatorT{destination_accounting});
        EXPECT_NE(destination.get_allocator(), source.get_allocator());
        EXPECT_EQ(source_accounting.allocated_bytes(), source.capacity() * sizeof(std::uint64_t));
        EXPECT_EQ(destination_accounting.allocated_bytes(), destination.capacity() * sizeof(std::uint64_t));
    }

    EXPECT_EQ(source_accounting.allocated_bytes(), 0);
    EXPECT_EQ(destination_accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, ContainerMoveAssignmentTransfersTheDomainWithTheStorage) {
    MemoryAccounting source_accounting;
    MemoryAccounting destination_accounting;

    {
        VectorT source({1, 2, 3}, AllocatorT{source_accounting});
        VectorT destination({9}, AllocatorT{destination_accounting});
        const auto source_bytes = source_accounting.allocated_bytes();
        const auto* source_storage = source.data();

        destination = std::move(source);
        EXPECT_EQ(destination.get_allocator(), AllocatorT{source_accounting});
        EXPECT_EQ(source.get_allocator(), AllocatorT{source_accounting});
        EXPECT_EQ(destination.data(), source_storage);
        EXPECT_EQ(destination.size(), 3);
        EXPECT_EQ(destination.front(), 1);
        EXPECT_EQ(destination.back(), 3);
        EXPECT_EQ(source_accounting.allocated_bytes(), source_bytes);
        EXPECT_EQ(destination_accounting.allocated_bytes(), 0);
    }

    EXPECT_EQ(source_accounting.allocated_bytes(), 0);
    EXPECT_EQ(destination_accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingTest, ContainerSwapExchangesDomainsWithTheirStorage) {
    MemoryAccounting first_accounting;
    MemoryAccounting second_accounting;

    {
        VectorT first({1, 2, 3}, AllocatorT{first_accounting});
        VectorT second({9}, AllocatorT{second_accounting});
        const auto first_bytes = first_accounting.allocated_bytes();
        const auto second_bytes = second_accounting.allocated_bytes();

        first.swap(second);
        EXPECT_EQ(first.get_allocator(), AllocatorT{second_accounting});
        EXPECT_EQ(second.get_allocator(), AllocatorT{first_accounting});
        EXPECT_EQ(first.size(), 1);
        EXPECT_EQ(first.front(), 9);
        EXPECT_EQ(second.size(), 3);
        EXPECT_EQ(second.front(), 1);
        EXPECT_EQ(first_accounting.allocated_bytes(), first_bytes);
        EXPECT_EQ(second_accounting.allocated_bytes(), second_bytes);
    }

    EXPECT_EQ(first_accounting.allocated_bytes(), 0);
    EXPECT_EQ(second_accounting.allocated_bytes(), 0);
}

TEST(MemoryAccountingDeathTest, AllocationRequestOverflowReportsContextBeforeTermination) {
    MemoryAccounting accounting;
    AllocatorT allocator(accounting);
    auto* live = allocator.allocate(2);
    const auto count = std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) + 1;
    const auto pattern = diagnostic_pattern("allocate", "request byte count overflow", count,
        sizeof(std::uint64_t), 2 * sizeof(std::uint64_t));

    EXPECT_EXIT(static_cast<void>(allocator.allocate(count)), testing::KilledBySignal(SIGABRT), pattern);

    allocator.deallocate(live, 2);
}

TEST(MemoryAccountingDeathTest, CumulativeOverflowIsRejectedBeforeRequestingHugeStorage) {
    MemoryAccounting accounting;
    CountingAllocator<std::byte> allocator(accounting);
    auto* live = allocator.allocate(8);
    const auto count = std::numeric_limits<std::size_t>::max();
    const auto pattern = diagnostic_pattern("allocate", "allocated byte count overflow", count, 1, 8);

    EXPECT_EXIT(static_cast<void>(allocator.allocate(count)), testing::KilledBySignal(SIGABRT), pattern);

    allocator.deallocate(live, 8);
}

TEST(MemoryAccountingDeathTest, DeallocationRequestOverflowReportsContextBeforeTermination) {
    MemoryAccounting accounting;
    AllocatorT allocator(accounting);
    auto* live = allocator.allocate(2);
    const auto count = std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) + 1;
    const auto pattern = diagnostic_pattern("deallocate", "request byte count overflow", count,
        sizeof(std::uint64_t), 2 * sizeof(std::uint64_t));

    // The invalid count must be rejected before reaching the underlying deallocator.
    EXPECT_EXIT(allocator.deallocate(live, count), testing::KilledBySignal(SIGABRT), pattern);

    allocator.deallocate(live, 2);
}

TEST(MemoryAccountingDeathTest, DeallocationUnderflowReportsTheCurrentLiveBytes) {
    MemoryAccounting accounting;
    AllocatorT allocator(accounting);
    auto* live = allocator.allocate(2);
    const auto pattern = diagnostic_pattern("deallocate", "allocated byte count underflow", 3,
        sizeof(std::uint64_t), 2 * sizeof(std::uint64_t));

    // Only the child supplies a wrong count; the parent releases the original allocation correctly.
    EXPECT_EXIT(allocator.deallocate(live, 3), testing::KilledBySignal(SIGABRT), pattern);

    allocator.deallocate(live, 2);
}

}  // namespace
}  // namespace cracking_lsm::test
