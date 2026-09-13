// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <variant>

#include <gtest/gtest.h>

#include <cracking-lsm/run/codec/key_codec/native_key_codec.hpp>
#include <cracking-lsm/run/codec/meta_codec/native_meta_codec.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>

#include "run_test_support.hpp"

/** @file native_codec_test.cpp
 * @brief Tests factory constraints, physical metadata widths, and native field codecs.
 */

namespace cracking_lsm::test {
namespace {

/** @brief Detects the payload-taking factory without instantiating a forbidden call. */
template <bool KeyOnly>
concept HasPayloadFactory = requires {
    KVEntry<std::uint64_t, KeyOnly>::make(1, PayloadRefT{2}, VersionT{3});
};

/** @brief Detects the key-only factory independently of the payload-taking overload. */
template <bool KeyOnly>
concept HasKeyOnlyFactory = requires { KVEntry<std::uint64_t, KeyOnly>::make(1, VersionT{3}); };

/** @brief Detects accidental reintroduction of the removed physical-field factory. */
template <bool KeyOnly>
concept HasPhysicalFactory = requires(EntryMeta<KeyOnly> metadata) {
    KVEntry<std::uint64_t, KeyOnly>::make(1, metadata);
};

static_assert(HasPayloadFactory<false> && !HasPayloadFactory<true>);
static_assert(HasKeyOnlyFactory<true> && !HasKeyOnlyFactory<false>);
static_assert(!HasPhysicalFactory<false> && !HasPhysicalFactory<true>);
static_assert(sizeof(EntryMeta<false>) == 16 && EntryMeta<false>::PhysicalBytes == 16);
static_assert(sizeof(EntryMeta<true>) == 8 && EntryMeta<true>::PhysicalBytes == 8);
static_assert(std::is_same_v<decltype(EntryMeta<true>::payload_ref), std::monostate>);
static_assert(std::is_trivially_copyable_v<EntryMeta<false>>);
static_assert(std::is_trivially_copyable_v<EntryMeta<true>>);
static_assert(MetaCodec<NativeMetaCodec<false>, false> && MetaCodec<NativeMetaCodec<true>, true>);
static_assert(!MetaCodec<NativeMetaCodec<false>, true> && !MetaCodec<NativeMetaCodec<true>, false>);

/** @brief Runs metadata and entry tests for both payload modes. */
template <typename ModeT>
class NativeMetaCodecTest : public testing::Test, public RunTestData<ModeT::value> {};

TYPED_TEST_SUITE(NativeMetaCodecTest, PayloadModes);

/** @brief Verifies version limits, kind packing, tombstones, and mode-specific factories. */
TYPED_TEST(NativeMetaCodecTest, FactoriesEnforceVersionBoundaryAndKind) {
    using EntryT = typename TestFixture::EntryT;
    using MetaT = typename TestFixture::MetaT;
    for (const auto version : {VersionT{0}, MetaT::MaxVersion}) {
        const auto entry = TestFixture::make_entry(42, version);
        EXPECT_EQ(entry.key, 42U);
        EXPECT_EQ(entry.version(), version);
        EXPECT_EQ(entry.kind(), EntryKindT::valid);
        EXPECT_EQ(entry.metadata.version_and_kind, version << 8);
        EXPECT_TRUE(entry.metadata.is_valid_kind());
        const auto deleted = EntryT::tombstone(42, version);
        EXPECT_EQ(deleted.version(), version);
        EXPECT_EQ(deleted.kind(), EntryKindT::tombstone);
        EXPECT_TRUE(deleted.metadata.is_valid_kind());
        if constexpr (!TypeParam::value) {
            EXPECT_EQ(deleted.metadata.payload_ref, 0U);
        }
    }
    EXPECT_THROW((void)TestFixture::make_entry(42, MetaT::MaxVersion + 1), std::invalid_argument);
    EXPECT_THROW((void)EntryT::tombstone(42, MetaT::MaxVersion + 1), std::invalid_argument);
}

/** @brief Checks native single-item bytes, unaligned decoding, and untouched surrounding bytes. */
TYPED_TEST(NativeMetaCodecTest, SingleItemRoundTripPreservesGuardsAndNativeBytes) {
    using CodecT = NativeMetaCodec<TypeParam::value>;
    using MutBytesViewT = emds::common::MutBytesViewT;
    const auto entry = TestFixture::make_entry(7, 0x01020304050607ULL, 0x8877665544332211ULL);
    alignas(std::uint64_t) std::array<std::byte, CodecT::EncodedBytes + 9> bytes;
    bytes.fill(std::byte{0xA5});
    const auto output = MutBytesViewT{bytes}.subspan(1, CodecT::EncodedBytes);
    CodecT::encode(entry.metadata, output);
    EXPECT_EQ(std::memcmp(output.data(), &entry.metadata.version_and_kind, 8), 0);
    TestFixture::expect_meta(CodecT::decode(output), entry.metadata);
    EXPECT_EQ(bytes.front(), std::byte{0xA5});
    for (std::size_t i = CodecT::EncodedBytes + 1; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], std::byte{0xA5});
    }
    if constexpr (!TypeParam::value) {
        EXPECT_EQ(std::memcmp(output.data() + 8, &entry.metadata.payload_ref, 8), 0);
    }
}

/** @brief Checks one-pass batch order and preservation of tombstone payload bytes and raw invalid kinds. */
TYPED_TEST(NativeMetaCodecTest, BatchRoundTripPreservesEveryPhysicalField) {
    using CodecT = NativeMetaCodec<TypeParam::value>;
    using MetaT = typename TestFixture::MetaT;
    using MutBytesViewT = emds::common::MutBytesViewT;
    const auto entries = TestFixture::make_entries(3);
    std::array<MetaT, 3> original{entries[0].metadata, entries[1].metadata, entries[2].metadata};
    if constexpr (!TypeParam::value) {
        original[1].payload_ref = 987654321;
    }
    original[2].version_and_kind = (VersionT{9} << 8) | 0xFF;
    alignas(std::uint64_t) std::array<std::byte, 3 * CodecT::EncodedBytes + 2> bytes;
    bytes.fill(std::byte{0xA5});
    const auto output = MutBytesViewT{bytes}.subspan(1, 3 * CodecT::EncodedBytes);
    CodecT::encode_batch(original, output);
    std::array<MetaT, 3> decoded{};
    CodecT::decode_batch(output, decoded);
    for (std::size_t i = 0; i < original.size(); ++i) {
        TestFixture::expect_meta(decoded[i], original[i]);
        TestFixture::expect_meta(CodecT::decode(output.subspan(i * CodecT::EncodedBytes)), original[i]);
    }
    EXPECT_FALSE(decoded[2].is_valid_kind());
    EXPECT_EQ(bytes.front(), std::byte{0xA5});
    EXPECT_EQ(bytes.back(), std::byte{0xA5});
}

/** @brief Verifies empty batches and rejection before mutation when a byte span is too short. */
TYPED_TEST(NativeMetaCodecTest, EmptyAndShortBuffersAreHandledBeforeMutation) {
    using CodecT = NativeMetaCodec<TypeParam::value>;
    using MetaT = typename TestFixture::MetaT;
    using MutBytesViewT = emds::common::MutBytesViewT;
    EXPECT_NO_THROW(CodecT::encode_batch({}, {}));
    EXPECT_NO_THROW(CodecT::decode_batch({}, {}));
    const auto metadata = TestFixture::make_entry(1, 7).metadata;
    const std::array<MetaT, 2> original{metadata, metadata};
    std::array<std::byte, 2 * CodecT::EncodedBytes> bytes;
    bytes.fill(std::byte{0xA5});
    const auto before = bytes;
    const auto short_item = MutBytesViewT{bytes}.first(CodecT::EncodedBytes - 1);
    const auto short_batch = MutBytesViewT{bytes}.first(bytes.size() - 1);
    EXPECT_THROW(CodecT::encode(metadata, short_item), std::invalid_argument);
    EXPECT_THROW((void)CodecT::decode(short_item), std::invalid_argument);
    EXPECT_THROW(CodecT::encode_batch(original, short_batch), std::invalid_argument);
    EXPECT_EQ(bytes, before);
    auto decoded = original;
    EXPECT_THROW(CodecT::decode_batch(short_batch, decoded), std::invalid_argument);
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        TestFixture::expect_meta(decoded[i], original[i]);
    }
}

/** @brief Verifies native key bytes, contiguous batches, unaligned access, and byte guards. */
TEST(NativeKeyCodecTest, SingleAndBatchRoundTripNativeBytes) {
    using KeyT = std::uint64_t;
    using CodecT = NativeKeyCodec<KeyT>;
    using MutBytesViewT = emds::common::MutBytesViewT;
    const std::array<KeyT, 3> keys{0x8877665544332211ULL, 0, std::numeric_limits<KeyT>::max()};
    alignas(KeyT) std::array<std::byte, sizeof(keys) + 2> bytes;
    bytes.fill(std::byte{0xA5});
    const auto output = MutBytesViewT{bytes}.subspan(1, sizeof(keys));
    CodecT::encode(keys[0], output);
    EXPECT_EQ(CodecT::decode(output), keys[0]);
    CodecT::encode_batch(keys, output);
    EXPECT_EQ(std::memcmp(output.data(), keys.data(), sizeof(keys)), 0);
    std::array<KeyT, 3> decoded{};
    CodecT::decode_batch(output, decoded);
    EXPECT_EQ(decoded, keys);
    EXPECT_EQ(bytes.front(), std::byte{0xA5});
    EXPECT_EQ(bytes.back(), std::byte{0xA5});
}

/** @brief Checks empty key batches and short spans without changing caller-owned outputs. */
TEST(NativeKeyCodecTest, EmptyAndShortBuffersAreHandledBeforeMutation) {
    using CodecT = NativeKeyCodec<std::uint64_t>;
    using MutBytesViewT = emds::common::MutBytesViewT;
    EXPECT_NO_THROW(CodecT::encode_batch({}, {}));
    EXPECT_NO_THROW(CodecT::decode_batch({}, {}));
    const std::array<std::uint64_t, 2> keys{42, 99};
    std::array<std::byte, sizeof(keys)> bytes{};
    const auto before = bytes;
    const auto short_item = MutBytesViewT{bytes}.first(CodecT::EncodedBytes - 1);
    const auto short_batch = MutBytesViewT{bytes}.first(bytes.size() - 1);
    EXPECT_THROW(CodecT::encode(keys[0], short_item), std::invalid_argument);
    EXPECT_THROW((void)CodecT::decode(short_item), std::invalid_argument);
    EXPECT_THROW(CodecT::encode_batch(keys, short_batch), std::invalid_argument);
    EXPECT_EQ(bytes, before);
    auto decoded = keys;
    EXPECT_THROW(CodecT::decode_batch(short_batch, decoded), std::invalid_argument);
    EXPECT_EQ(decoded, keys);
}

}  // namespace
}  // namespace cracking_lsm::test
