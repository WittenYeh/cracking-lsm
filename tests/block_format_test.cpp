// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

#include <gtest/gtest.h>

#include <cracking-lsm/run/block/block_format.hpp>
#include <cracking-lsm/run/block/block_view.hpp>

#include "run_test_support.hpp"

/** @file block_format_test.cpp
 * @brief Tests headerless block layout, tail growth, ordinal access, and malformed input.
 */

namespace cracking_lsm::test {
namespace {

/** @brief Shares a small fixed block size and entry generators across payload modes. */
template <typename ModeT>
class BlockFormatTest : public testing::Test, public RunTestData<ModeT::value> {
public:
    using FormatT = BlockFormat<std::uint64_t, ModeT::value>;
    using ViewT = BlockView<std::uint64_t, ModeT::value>;
    using MutBytesViewT = emds::common::MutBytesViewT;
    static constexpr std::size_t BlockBytes = 4096;
};

TYPED_TEST_SUITE(BlockFormatTest, PayloadModes);

/** @brief Verifies empty/single/full blocks, exact column bytes, guards, and unaligned ordinal reads. */
TYPED_TEST(BlockFormatTest, NativeLayoutAndOrdinalReadsMatchEntries) {
    using FormatT = typename TestFixture::FormatT;
    using ViewT = typename TestFixture::ViewT;
    using MutBytesViewT = typename TestFixture::MutBytesViewT;
    constexpr auto BlockBytes = TestFixture::BlockBytes;
    constexpr auto Capacity = BlockBytes / (8 + (TypeParam::value ? 8 : 16));
    EXPECT_EQ(FormatT::max_entries(BlockBytes), Capacity);
    EXPECT_EQ(FormatT::max_entries(0), 0U);
    alignas(std::uint64_t) std::array<std::byte, BlockBytes + 2> bytes;
    for (const auto count : {std::size_t{0}, std::size_t{1}, Capacity}) {
        SCOPED_TRACE(count);
        bytes.fill(std::byte{0xA5});
        const auto block = MutBytesViewT{bytes}.subspan(1, BlockBytes);
        const auto entries = TestFixture::make_entries(count);
        const auto descriptor = FormatT::encode_block(entries, block);
        EXPECT_EQ(descriptor.encoding_strategy, KeyEncodingStrategyT::native_fixed);
        EXPECT_EQ(descriptor.num_entries, count);
        EXPECT_EQ(descriptor.meta_offset, count * 8);
        EXPECT_EQ(descriptor.encoded_keys_bytes, count * 8);
        EXPECT_EQ(descriptor.encoded_block_bytes, count * (TypeParam::value ? 16 : 24));
        TestFixture::expect_block_bytes(block, entries);
        const auto view = ViewT::make(block, descriptor);
        EXPECT_EQ(view.size(), entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            EXPECT_EQ(view.key_at(i), entries[i].key);
            TestFixture::expect_meta(view.meta_at(i), entries[i].metadata);
            TestFixture::expect_entry(view.entry_at(i), entries[i]);
        }
        EXPECT_EQ(bytes.front(), std::byte{0xA5});
        EXPECT_EQ(bytes.back(), std::byte{0xA5});
    }
}

/** @brief Verifies repeated tail growth preserves existing columns, padding, and empty-append behavior. */
TYPED_TEST(BlockFormatTest, TailAppendRelocatesMetadataWithoutChangingEntries) {
    using FormatT = typename TestFixture::FormatT;
    using ViewT = typename TestFixture::ViewT;
    using EntryT = typename TestFixture::EntryT;
    std::array<std::byte, TestFixture::BlockBytes> block{};
    const auto entries = TestFixture::make_entries(FormatT::max_entries(block.size()));
    const std::span<const EntryT> input = entries;
    auto descriptor = FormatT::encode_block(input.first(1), block);
    std::size_t consumed = 1;
    for (const auto count : {std::size_t{1}, entries.size() - 2}) {
        descriptor = FormatT::append_block(input.subspan(consumed, count), block, descriptor);
        consumed += count;
        TestFixture::expect_block_bytes(block, input.first(consumed));
        const auto view = ViewT::make(block, descriptor);
        ASSERT_EQ(view.size(), consumed);
        for (std::size_t i = 0; i < consumed; ++i) {
            TestFixture::expect_entry(view.entry_at(i), entries[i]);
        }
    }
    const auto before = block;
    const auto unchanged = FormatT::append_block({}, block, descriptor);
    EXPECT_EQ(unchanged.num_entries, descriptor.num_entries);
    EXPECT_EQ(unchanged.encoded_block_bytes, descriptor.encoded_block_bytes);
    EXPECT_EQ(block, before);
}

/** @brief Rejects oversized batches and invalid kinds before any encode or append modifies output. */
TYPED_TEST(BlockFormatTest, InvalidInputLeavesBlockUnchanged) {
    using FormatT = typename TestFixture::FormatT;
    std::array<std::byte, TestFixture::BlockBytes> block{};
    const auto original = TestFixture::make_entries(2);
    const auto descriptor = FormatT::encode_block(original, block);
    const auto before = block;
    const auto oversized = TestFixture::make_entries(FormatT::max_entries(block.size()) + 1);
    auto invalid = original;
    invalid.back().metadata.version_and_kind = 0xFF;
    EXPECT_THROW((void)FormatT::encode_block(oversized, block), std::invalid_argument);
    EXPECT_THROW((void)FormatT::encode_block(invalid, block), std::invalid_argument);
    EXPECT_THROW((void)FormatT::append_block(oversized, block, descriptor), std::invalid_argument);
    EXPECT_THROW((void)FormatT::append_block(invalid, block, descriptor), std::invalid_argument);
    EXPECT_THROW((void)FormatT::describe_block(std::numeric_limits<std::size_t>::max(), block.size()),
        std::invalid_argument);
    EXPECT_EQ(block, before);
}

/** @brief Rejects inconsistent descriptors, truncated spans, and descriptors from the other payload mode. */
TYPED_TEST(BlockFormatTest, RejectsMalformedDescriptorsAndWrongPayloadMode) {
    using FormatT = typename TestFixture::FormatT;
    using ViewT = typename TestFixture::ViewT;
    using OtherViewT = BlockView<std::uint64_t, !TypeParam::value>;
    using ByteViewT = typename TestFixture::ByteViewT;
    std::array<std::byte, TestFixture::BlockBytes> block{};
    const auto entries = TestFixture::make_entries(3);
    const auto descriptor = FormatT::encode_block(entries, block);
    std::array<BlockDescriptor, 6> invalid;
    invalid.fill(descriptor);
    invalid[0].encoding_strategy = static_cast<KeyEncodingStrategyT>(0xFF);
    invalid[1].num_entries = std::numeric_limits<std::size_t>::max();
    --invalid[2].encoded_keys_bytes;
    ++invalid[3].meta_offset;
    --invalid[4].encoded_block_bytes;
    invalid[5].encoded_block_bytes = block.size() + 1;
    const auto before = block;
    for (const auto& broken : invalid) {
        EXPECT_THROW((void)ViewT::make(block, broken), std::invalid_argument);
        EXPECT_THROW((void)FormatT::append_block({}, block, broken), std::invalid_argument);
    }
    EXPECT_THROW((void)ViewT::make(ByteViewT{block}.first(descriptor.encoded_block_bytes - 1), descriptor),
        std::invalid_argument);
    EXPECT_THROW((void)OtherViewT::make(block, descriptor), std::invalid_argument);
    EXPECT_EQ(block, before);
}

/** @brief Rejects out-of-range ordinals on empty and nonempty views before computing byte offsets. */
TYPED_TEST(BlockFormatTest, RejectsOutOfRangeOrdinals) {
    using FormatT = typename TestFixture::FormatT;
    using ViewT = typename TestFixture::ViewT;
    std::array<std::byte, TestFixture::BlockBytes> block{};
    for (const auto count : {std::size_t{0}, std::size_t{2}}) {
        const auto entries = TestFixture::make_entries(count);
        const auto descriptor = FormatT::encode_block(entries, block);
        const auto view = ViewT::make(block, descriptor);
        for (const auto i : {count, std::numeric_limits<std::size_t>::max()}) {
            EXPECT_THROW((void)view.key_at(i), std::invalid_argument);
            EXPECT_THROW((void)view.meta_at(i), std::invalid_argument);
            EXPECT_THROW((void)view.entry_at(i), std::invalid_argument);
        }
    }
}

/** @brief Verifies raw metadata can be decoded but an unsupported kind cannot become a logical entry. */
TYPED_TEST(BlockFormatTest, EntryReadRejectsCorruptKind) {
    using FormatT = typename TestFixture::FormatT;
    using ViewT = typename TestFixture::ViewT;
    std::array<std::byte, TestFixture::BlockBytes> block{};
    const auto entries = TestFixture::make_entries(2);
    const auto descriptor = FormatT::encode_block(entries, block);
    const std::uint64_t corrupt_word = (VersionT{7} << 8) | 0xFF;
    std::memcpy(block.data() + descriptor.meta_offset, &corrupt_word, sizeof(corrupt_word));
    const auto view = ViewT::make(block, descriptor);
    EXPECT_FALSE(view.meta_at(0).is_valid_kind());
    EXPECT_THROW((void)view.entry_at(0), std::invalid_argument);
    TestFixture::expect_entry(view.entry_at(1), entries[1]);
}

/** @brief Verifies a three-byte physical key does not introduce C++ entry padding into a block. */
TYPED_TEST(BlockFormatTest, CustomKeyWidthKeepsMetadataPackedAndUnaligned) {
    using KeyT = std::array<std::byte, 3>;
    using EntryT = KVEntry<KeyT, TypeParam::value>;
    using FormatT = BlockFormat<KeyT, TypeParam::value>;
    using ViewT = BlockView<KeyT, TypeParam::value>;
    const auto metadata = TestFixture::make_entry(1, 7).metadata;
    const std::array entries{EntryT{.key = {std::byte{1}, std::byte{2}, std::byte{3}}, .metadata = metadata}};
    alignas(std::uint64_t) std::array<std::byte, 64> block{};
    const auto descriptor = FormatT::encode_block(entries, block);
    EXPECT_EQ(descriptor.meta_offset, 3U);
    EXPECT_EQ(descriptor.encoded_block_bytes, TypeParam::value ? 11U : 19U);
    EXPECT_GT(sizeof(EntryT), descriptor.encoded_block_bytes);
    EXPECT_EQ(std::memcmp(block.data(), entries[0].key.data(), 3), 0);
    EXPECT_EQ(std::memcmp(block.data() + 3, &metadata.version_and_kind, 8), 0);
    const auto decoded = ViewT::make(block, descriptor).entry_at(0);
    EXPECT_EQ(decoded.key, entries[0].key);
    TestFixture::expect_meta(decoded.metadata, metadata);
}

}  // namespace
}  // namespace cracking_lsm::test
