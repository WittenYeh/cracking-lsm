// Copyright 2026 Weitang Ye
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <gtest/gtest.h>

#include <cracking-lsm/run/run_file.hpp>

#include "run_test_support.hpp"

/** @file run_file_test.cpp
 * @brief Tests real Direct I/O, append boundaries, failure states, and working-file ownership.
 */

namespace cracking_lsm::test {
namespace {

static_assert(std::is_class_v<detail::RunFile<std::uint64_t>>);

/** @brief Temporarily forces file writes to fail with EFBIG within this test process only. */
class ScopedWriteFailure {
public:
    /** @brief Saves process state, ignores SIGXFSZ, and sets the soft file-size limit to zero. */
    ScopedWriteFailure() {
        if (::getrlimit(RLIMIT_FSIZE, &saved_limit_) != 0) {
            throw std::system_error(errno, std::generic_category(), "getrlimit failed");
        }
        saved_handler_ = std::signal(SIGXFSZ, SIG_IGN);
        if (saved_handler_ == SIG_ERR) {
            throw std::system_error(errno, std::generic_category(), "signal setup failed");
        }
        auto limited = saved_limit_;
        limited.rlim_cur = 0;
        if (::setrlimit(RLIMIT_FSIZE, &limited) != 0) {
            const auto saved_errno = errno;
            std::signal(SIGXFSZ, saved_handler_);
            throw std::system_error(saved_errno, std::generic_category(), "setrlimit failed");
        }
    }

    /** @brief Restores the original soft limit and signal handler on every exit path. */
    ~ScopedWriteFailure() {
        EXPECT_EQ(::setrlimit(RLIMIT_FSIZE, &saved_limit_), 0);
        EXPECT_NE(std::signal(SIGXFSZ, saved_handler_), SIG_ERR);
    }

    ScopedWriteFailure(const ScopedWriteFailure&) = delete;
    auto operator=(const ScopedWriteFailure&) -> ScopedWriteFailure& = delete;

private:
    /** @brief Original resource limits, including the unchanged hard limit. */
    rlimit saved_limit_{};
    /** @brief Signal disposition to restore after the forced failure. */
    void (*saved_handler_)(int) = SIG_DFL;
};

/** @brief Provides an isolated mkdtemp directory and a real working file for each payload mode. */
template <typename ModeT>
class RunFileTest : public testing::Test, public RunTestData<ModeT::value> {
public:
    using EntryT = typename RunTestData<ModeT::value>::EntryT;
    using FileT = detail::RunFile<std::uint64_t, ModeT::value>;
    static constexpr std::size_t BlockBytes = 4096;

    static_assert(!std::is_copy_constructible_v<FileT> && !std::is_copy_assignable_v<FileT>);
    static_assert(std::is_nothrow_move_constructible_v<FileT>);
    static_assert(std::is_nothrow_move_assignable_v<FileT>);

    /** @brief Creates a unique directory without accessing any existing user data. */
    auto SetUp() -> void override {
        auto path_template =
            (std::filesystem::temp_directory_path() / "cracking-lsm-run-test-XXXXXX").string();
        const auto* created = ::mkdtemp(path_template.data());
        if (created == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp failed");
        }
        test_directory_ = created;
    }

    /** @brief Removes only the temporary directory successfully created by this test. */
    auto TearDown() -> void override {
        if (!test_directory_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(test_directory_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    /** @brief Returns a path inside the test-owned directory without creating a file. */
    auto file_path(const std::string& name = "run.bin") const -> std::filesystem::path {
        return test_directory_ / name;
    }

    /** @brief Reads persisted bytes independently of the RunFile's aligned I/O buffer. */
    static auto read_file(const std::filesystem::path& path) -> std::vector<std::byte> {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot read test working file");
        }
        std::vector<std::byte> bytes(std::filesystem::file_size(path));
        if (!bytes.empty()) {
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
                throw std::runtime_error("incomplete independent file read");
            }
        }
        return bytes;
    }

    /** @brief Materializes scan results only in test code for full field/order comparisons. */
    static auto collect(const FileT& run_file) -> std::vector<EntryT> {
        std::vector<EntryT> entries;
        run_file.scan([&entries](const EntryT& entry) { entries.push_back(entry); });
        return entries;
    }

    /** @brief Checks that failed, moved-from, or cleared objects reject all non-cleanup operations. */
    static auto expect_unusable(FileT& run_file) -> void {
        EXPECT_THROW((void)run_file.size(), std::logic_error);
        EXPECT_THROW((void)run_file.empty(), std::logic_error);
        EXPECT_THROW((void)run_file.block_capacity(), std::logic_error);
        EXPECT_THROW(run_file.append({}), std::logic_error);
        EXPECT_THROW(run_file.scan([](const EntryT&) {}), std::logic_error);
    }

private:
    /** @brief Exact test-owned directory; left empty if setup fails. */
    std::filesystem::path test_directory_;
};

TYPED_TEST_SUITE(RunFileTest, PayloadModes);

/** @brief Checks factory validation, empty state, empty append, and idempotent permanent clear. */
TYPED_TEST(RunFileTest, CreateEmptyValidateOptionsAndClear) {
    using FileT = typename TestFixture::FileT;
    const auto path = this->file_path();
    EXPECT_THROW((void)FileT::create({}, TestFixture::BlockBytes), std::invalid_argument);
    for (const auto block_bytes : {0U, 1U, 4095U, 4097U}) {
        EXPECT_THROW((void)FileT::create(path, block_bytes), std::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(path));
    }
    auto run_file = FileT::create(path, TestFixture::BlockBytes);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(run_file.empty());
    EXPECT_EQ(run_file.size(), 0U);
    EXPECT_EQ(run_file.block_capacity(), TestFixture::BlockBytes / (TypeParam::value ? 16 : 24));
    EXPECT_NO_THROW(run_file.append({}));
    EXPECT_TRUE(TestFixture::collect(run_file).empty());
    EXPECT_EQ(std::filesystem::file_size(path), 0U);
    run_file.clear();
    EXPECT_FALSE(std::filesystem::exists(path));
    TestFixture::expect_unusable(run_file);
    EXPECT_NO_THROW(run_file.clear());
}

/** @brief Checks full blocks, tail rewrites, cross-block batches, disk bytes, and physical scan order. */
TYPED_TEST(RunFileTest, MultipleBatchesRoundTripAtTwoBlockSizes) {
    using FileT = typename TestFixture::FileT;
    using EntryT = typename TestFixture::EntryT;
    using ByteViewT = typename TestFixture::ByteViewT;
    for (const auto block_bytes : {std::size_t{4096}, std::size_t{65536}}) {
        SCOPED_TRACE(block_bytes);
        const auto path = this->file_path("run-" + std::to_string(block_bytes));
        auto run_file = FileT::create(path, block_bytes);
        const auto capacity = run_file.block_capacity();
        const auto entries = TestFixture::make_entries(3 * capacity + 3);
        const std::span<const EntryT> input = entries;
        std::size_t consumed = 0;
        for (const auto count : std::array<std::size_t, 6>{1, capacity - 1, 2, capacity, capacity - 1, 2}) {
            run_file.append(input.subspan(consumed, count));
            consumed += count;
            run_file.append({});
            EXPECT_EQ(run_file.size(), consumed);
            EXPECT_FALSE(run_file.empty());
            TestFixture::expect_entries(TestFixture::collect(run_file), input.first(consumed));
            const auto num_blocks = (consumed + capacity - 1) / capacity;
            ASSERT_EQ(std::filesystem::file_size(path), num_blocks * block_bytes);
            const auto bytes = TestFixture::read_file(path);
            for (std::size_t i = 0; i < num_blocks; ++i) {
                const auto block_entries = std::min(capacity, consumed - i * capacity);
                TestFixture::expect_block_bytes(ByteViewT{bytes}.subspan(i * block_bytes, block_bytes),
                    input.subspan(i * capacity, block_entries));
            }
        }
        EXPECT_EQ(consumed, entries.size());
    }
}

/** @brief Verifies invalid entries and oversized batches leave both the file and usable state unchanged. */
TYPED_TEST(RunFileTest, RejectedAppendPreservesFileAndAllowsRetry) {
    using FileT = typename TestFixture::FileT;
    const auto path = this->file_path();
    auto run_file = FileT::create(path, TestFixture::BlockBytes);
    const auto original = TestFixture::make_entries(2);
    run_file.append(original);
    const auto before = TestFixture::read_file(path);
    const auto oversized = TestFixture::make_entries(run_file.block_capacity() + 1);
    auto invalid = TestFixture::make_entries(2, 2);
    invalid.back().metadata.version_and_kind = 0xFF;
    EXPECT_THROW(run_file.append(oversized), std::invalid_argument);
    EXPECT_THROW(run_file.append(invalid), std::invalid_argument);
    EXPECT_EQ(run_file.size(), original.size());
    EXPECT_EQ(TestFixture::read_file(path), before);
    TestFixture::expect_entries(TestFixture::collect(run_file), original);
    EXPECT_NO_THROW(run_file.append(TestFixture::make_entries(1, 2)));
    EXPECT_EQ(run_file.size(), 3U);
}

/** @brief Verifies exclusive create preserves an existing file's contents. */
TYPED_TEST(RunFileTest, ExistingFileIsNeverOverwrittenOrRemoved) {
    using FileT = typename TestFixture::FileT;
    const auto path = this->file_path();
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing-data-must-survive";
        ASSERT_TRUE(output.good());
    }
    const auto before = TestFixture::read_file(path);
    EXPECT_THROW((void)FileT::create(path, TestFixture::BlockBytes), std::system_error);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(TestFixture::read_file(path), before);
}

/** @brief Checks a real short block read permanently fails the run_file but still permits cleanup. */
TYPED_TEST(RunFileTest, TruncatedFileEntersFailedState) {
    using FileT = typename TestFixture::FileT;
    const auto path = this->file_path();
    auto run_file = FileT::create(path, TestFixture::BlockBytes);
    run_file.append(TestFixture::make_entries(3));
    std::filesystem::resize_file(path, TestFixture::BlockBytes / 2);
    try {
        static_cast<void>(TestFixture::collect(run_file));
        FAIL() << "truncated block was accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("short block read"), std::string::npos);
    }
    TestFixture::expect_unusable(run_file);
    EXPECT_NO_THROW(run_file.clear());
    EXPECT_FALSE(std::filesystem::exists(path));
}

/** @brief Checks an unsupported on-disk entry kind is rejected during scan and marks the run_file failed. */
TYPED_TEST(RunFileTest, CorruptKindEntersFailedState) {
    using FileT = typename TestFixture::FileT;
    const auto path = this->file_path();
    auto run_file = FileT::create(path, TestFixture::BlockBytes);
    run_file.append(TestFixture::make_entries(3));
    {
        std::fstream output(path, std::ios::in | std::ios::out | std::ios::binary);
        const std::uint64_t corrupt_word = 0xFF;
        output.seekp(3 * sizeof(std::uint64_t));
        output.write(reinterpret_cast<const char*>(&corrupt_word), sizeof(corrupt_word));
        ASSERT_TRUE(output.good());
    }
    EXPECT_THROW((void)TestFixture::collect(run_file), std::invalid_argument);
    TestFixture::expect_unusable(run_file);
    run_file.clear();
    EXPECT_FALSE(std::filesystem::exists(path));
}

/** @brief Injects an actual EFBIG write error without mocks and verifies failed-state cleanup. */
TYPED_TEST(RunFileTest, WriteFailureEntersFailedState) {
    using FileT = typename TestFixture::FileT;
    const auto path = this->file_path();
    auto run_file = FileT::create(path, TestFixture::BlockBytes);
    const auto entries = TestFixture::make_entries(3);
    {
        const ScopedWriteFailure failure;
        try {
            run_file.append(entries);
            FAIL() << "write unexpectedly succeeded with a zero file-size limit";
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::make_error_code(std::errc::file_too_large));
        }
    }
    TestFixture::expect_unusable(run_file);
    EXPECT_EQ(std::filesystem::file_size(path), 0U);
    run_file.clear();
    EXPECT_FALSE(std::filesystem::exists(path));
}

/** @brief Verifies move construction/assignment transfer ownership and destruction removes the owned file. */
TYPED_TEST(RunFileTest, MoveAndDestructionTransferAndReleaseFiles) {
    using FileT = typename TestFixture::FileT;
    const auto source_path = this->file_path("source");
    const auto target_path = this->file_path("target");
    const auto entries = TestFixture::make_entries(3);
    {
        auto source = FileT::create(source_path, TestFixture::BlockBytes);
        source.append(entries);
        auto moved = std::move(source);
        TestFixture::expect_unusable(source);
        EXPECT_NO_THROW(source.clear());
        EXPECT_TRUE(std::filesystem::exists(source_path));
        auto target = FileT::create(target_path, TestFixture::BlockBytes);
        target.append(TestFixture::make_entries(1));
        target = std::move(moved);
        EXPECT_FALSE(std::filesystem::exists(target_path));
        TestFixture::expect_unusable(moved);
        TestFixture::expect_entries(TestFixture::collect(target), entries);
    }
    EXPECT_FALSE(std::filesystem::exists(source_path));
    EXPECT_FALSE(std::filesystem::exists(target_path));
}

}  // namespace
}  // namespace cracking_lsm::test
