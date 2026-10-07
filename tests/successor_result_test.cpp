// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>

#include <gtest/gtest.h>

#include <cracking-lsm/engine/op_result/successor_result.hpp>

namespace cracking_lsm::test {
namespace {

template <typename ModeT>
class SuccessorResultTest : public ::testing::Test {
public:
    using ResultT = SuccessorResult<std::uint64_t, ModeT::value>;
    using EntryT = typename ResultT::EntryT;

    static auto make_entry() -> EntryT {
        if constexpr (ModeT::value) {
            return EntryT::make(42, 7);
        } else {
            return EntryT::make(42, 99, 7);
        }
    }
};

using KeyModes = ::testing::Types<std::false_type, std::true_type>;
TYPED_TEST_SUITE(SuccessorResultTest, KeyModes);

TYPED_TEST(SuccessorResultTest, DefaultResultIsNotFoundAndIdentifiesItsOperation) {
    const typename TestFixture::ResultT result;
    const OpResult& operation = result;
    EXPECT_EQ(operation.operation(), OpKind::successor);
    EXPECT_EQ(result.state(), QueryState::not_found);
    EXPECT_FALSE(result.entry());
}

TYPED_TEST(SuccessorResultTest, OwnsItsEntryAfterTheInputChangesAndIsDestroyed) {
    const auto result = [] {
        auto entry = TestFixture::make_entry();
        const typename TestFixture::ResultT owned(entry);
        entry.key = 123;
        entry.metadata.version_and_kind = 0;
        if constexpr (!TypeParam::value) {
            entry.metadata.payload_ref = 0;
        }
        return owned;
    }();

    const OpResult& operation = result;
    EXPECT_EQ(operation.operation(), OpKind::successor);
    EXPECT_EQ(result.state(), QueryState::value);
    ASSERT_TRUE(result.entry());
    EXPECT_EQ(result.entry()->key, 42);
    EXPECT_EQ(result.entry()->version(), 7);
    EXPECT_EQ(result.entry()->kind(), EntryKindT::valid);
    if constexpr (!TypeParam::value) {
        EXPECT_EQ(result.entry()->metadata.payload_ref, 99);
    }
}

TYPED_TEST(SuccessorResultTest, RejectsTombstonesAndUnsupportedKinds) {
    using ResultT = typename TestFixture::ResultT;
    using EntryT = typename TestFixture::EntryT;
    EXPECT_THROW(static_cast<void>(ResultT{EntryT::tombstone(42, 7)}), std::invalid_argument);
    for (const std::uint64_t kind : {2, 255}) {
        auto entry = TestFixture::make_entry();
        entry.metadata.version_and_kind &= ~EntryMeta<TypeParam::value>::KindMask;
        entry.metadata.version_and_kind |= kind;
        EXPECT_THROW(static_cast<void>(ResultT{entry}), std::invalid_argument);
    }
}

}  // namespace
}  // namespace cracking_lsm::test
