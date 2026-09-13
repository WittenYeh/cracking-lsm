// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include <emds-toolkit/common/byte_view.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

/** @file run_test_support.hpp
 * @brief Shared entry generators and independent physical-layout checks for Step 6 tests.
 */

namespace cracking_lsm::test {

using PayloadModes = testing::Types<std::false_type, std::true_type>;

/** @brief Supplies identical test scenarios for ordinary and key-only entries. */
template <bool KeyOnly>
struct RunTestData {
    using KeyT = std::uint64_t;
    using EntryT = KVEntry<KeyT, KeyOnly>;
    using MetaT = EntryMeta<KeyOnly>;
    using ByteViewT = emds::common::ByteView;

    /** @brief Creates a valid entry using the factory enabled for this payload mode. */
    static auto make_entry(KeyT key, VersionT version, PayloadRefT payload_ref = 123) -> EntryT {
        if constexpr (KeyOnly) {
            return EntryT::make(key, version);
        } else {
            return EntryT::make(key, payload_ref, version);
        }
    }

    /** @brief Produces unsorted entries with repeated keys, distinct versions, and tombstones. */
    static auto make_entries(std::size_t count, std::size_t start = 0) -> std::vector<EntryT> {
        std::vector<EntryT> entries;
        entries.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto ordinal = start + i;
            const auto key = static_cast<KeyT>((ordinal * 17 + 5) % 31);
            const auto version = static_cast<VersionT>(ordinal + 1);
            entries.push_back(ordinal % 3 == 1 ? EntryT::tombstone(key, version)
                                             : make_entry(key, version, 1000 + ordinal));
        }
        return entries;
    }

    /** @brief Compares physical fields, including the ordinary-mode tombstone placeholder. */
    static auto expect_meta(const MetaT& actual, const MetaT& expected) -> void {
        EXPECT_EQ(actual.version_and_kind, expected.version_and_kind);
        if constexpr (!KeyOnly) {
            EXPECT_EQ(actual.payload_ref, expected.payload_ref);
        }
    }

    /** @brief Compares complete entries without comparing C++ object padding. */
    static auto expect_entry(const EntryT& actual, const EntryT& expected) -> void {
        EXPECT_EQ(actual.key, expected.key);
        expect_meta(actual.metadata, expected.metadata);
    }

    /** @brief Verifies an ordered sequence of reconstructed entries. */
    static auto expect_entries(std::span<const EntryT> actual, std::span<const EntryT> expected) -> void {
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            SCOPED_TRACE(i);
            expect_entry(actual[i], expected[i]);
        }
    }

    /** @brief Checks raw column offsets and padding without using the production codec or descriptor. */
    static auto expect_block_bytes(ByteViewT block, std::span<const EntryT> entries) -> void {
        constexpr std::size_t MetaBytes = KeyOnly ? 8 : 16;
        const auto meta_offset = entries.size() * sizeof(KeyT);
        ASSERT_GE(block.size(), meta_offset + entries.size() * MetaBytes);
        std::vector<std::byte> expected(block.size(), std::byte{0});
        for (std::size_t i = 0; i < entries.size(); ++i) {
            std::memcpy(expected.data() + i * sizeof(KeyT), &entries[i].key, sizeof(KeyT));
            std::memcpy(expected.data() + meta_offset + i * MetaBytes,
                &entries[i].metadata.version_and_kind, sizeof(std::uint64_t));
            if constexpr (!KeyOnly) {
                std::memcpy(expected.data() + meta_offset + i * MetaBytes + 8,
                    &entries[i].metadata.payload_ref, sizeof(PayloadRefT));
            }
        }
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), block.begin()));
    }
};

}  // namespace cracking_lsm::test
