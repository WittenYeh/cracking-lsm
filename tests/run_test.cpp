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
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <gtest/gtest.h>

#include <emds-toolkit/io/direct_io_buffer.hpp>

#include <cracking-lsm/run/run.hpp>

#include "run_test_support.hpp"

/** @file run_test.cpp
 * @brief Tests real Direct I/O, append boundaries, failure states, and working-file ownership.
 */

namespace cracking_lsm::test {
namespace {

static_assert(std::is_class_v<Run<std::uint64_t>>);

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
class RunTest : public testing::Test, public RunTestData<ModeT::value> {
public:
    using EntryT = typename RunTestData<ModeT::value>::EntryT;
    using RunT = Run<std::uint64_t, std::less<>, ModeT::value>;
    using DirectIOBufferT = emds::io::DirectIOBuffer;
    static constexpr std::size_t BlockBytes = 4096;
    static constexpr std::size_t MaxEntries = std::numeric_limits<std::size_t>::max();

    static_assert(!std::is_copy_constructible_v<RunT> && !std::is_copy_assignable_v<RunT>);
    static_assert(std::is_nothrow_move_constructible_v<RunT>);
    static_assert(std::is_nothrow_move_assignable_v<RunT>);

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

    /** @brief Reads persisted bytes independently of the borrowed aligned I/O buffer. */
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
    auto collect(const RunT& run) -> std::vector<EntryT> {
        std::vector<EntryT> entries;
        run.scan(io_buffer_, [&entries](const EntryT& entry) { entries.push_back(entry); });
        return entries;
    }

    /** @brief Checks that failed, moved-from, or cleared objects reject all non-cleanup operations. */
    auto expect_unusable(RunT& run) -> void {
        EXPECT_THROW((void)run.size(), std::logic_error);
        EXPECT_THROW((void)run.empty(), std::logic_error);
        EXPECT_THROW((void)run.block_capacity(), std::logic_error);
        EXPECT_THROW(static_cast<void>(run.append_batch({}, io_buffer_)), std::logic_error);
        EXPECT_THROW(run.scan(io_buffer_, [](const EntryT&) {}), std::logic_error);
    }

protected:
    /** @brief One buffer for all Runs in this test, large enough for either tested block size. */
    DirectIOBufferT io_buffer_ = DirectIOBufferT::make(65536);

private:
    /** @brief Exact test-owned directory; left empty if setup fails. */
    std::filesystem::path test_directory_;
};

TYPED_TEST_SUITE(RunTest, PayloadModes);

/** @brief Checks factory validation, empty state, empty append, and idempotent permanent clear. */
TYPED_TEST(RunTest, CreateEmptyValidateOptionsAndClear) {
    using RunT = typename TestFixture::RunT;
    const auto path = this->file_path();
    EXPECT_THROW((void)RunT::create({},
        RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries}),
        std::invalid_argument);
    for (const auto block_bytes : {0U, 1U, 4095U, 4097U}) {
        EXPECT_THROW((void)RunT::create(path,
            RunOptions{.block_bytes = block_bytes, .max_entries = TestFixture::MaxEntries}),
            std::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(path));
    }
    auto run = RunT::create(path,
        RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries});
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(run.empty());
    EXPECT_EQ(run.size(), 0U);
    EXPECT_EQ(run.block_capacity(), TestFixture::BlockBytes / (TypeParam::value ? 16 : 24));
    EXPECT_NO_THROW(static_cast<void>(run.append_batch({}, this->io_buffer_)));
    EXPECT_TRUE(this->collect(run).empty());
    EXPECT_EQ(std::filesystem::file_size(path), 0U);
    run.clear();
    EXPECT_FALSE(std::filesystem::exists(path));
    this->expect_unusable(run);
    EXPECT_NO_THROW(run.clear());
}

/** @brief Checks full blocks, tail rewrites, cross-block batches, disk bytes, and physical scan order. */
TYPED_TEST(RunTest, MultipleBatchesRoundTripAtTwoBlockSizes) {
    using RunT = typename TestFixture::RunT;
    using EntryT = typename TestFixture::EntryT;
    using ByteViewT = typename TestFixture::ByteViewT;
    for (const auto block_bytes : {std::size_t{4096}, std::size_t{65536}}) {
        SCOPED_TRACE(block_bytes);
        const auto path = this->file_path("run-" + std::to_string(block_bytes));
        auto run = RunT::create(path,
            RunOptions{.block_bytes = block_bytes, .max_entries = TestFixture::MaxEntries});
        const auto capacity = run.block_capacity();
        const auto entries = TestFixture::make_entries(3 * capacity + 3);
        const std::span<const EntryT> input = entries;
        std::size_t consumed = 0;
        for (const auto count : std::array<std::size_t, 6>{1, capacity - 1, 2, capacity, capacity - 1, 2}) {
            static_cast<void>(run.append_batch(input.subspan(consumed, count), this->io_buffer_));
            consumed += count;
            static_cast<void>(run.append_batch({}, this->io_buffer_));
            EXPECT_EQ(run.size(), consumed);
            EXPECT_FALSE(run.empty());
            TestFixture::expect_entries(this->collect(run), input.first(consumed));
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
TYPED_TEST(RunTest, RejectedAppendPreservesFileAndAllowsRetry) {
    using RunT = typename TestFixture::RunT;
    const auto path = this->file_path();
    auto run = RunT::create(path,
        RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries});
    const auto original = TestFixture::make_entries(2);
    static_cast<void>(run.append_batch(original, this->io_buffer_));
    const auto before = TestFixture::read_file(path);
    const auto oversized = TestFixture::make_entries(run.block_capacity() + 1);
    auto invalid = TestFixture::make_entries(2, 2);
    invalid.back().metadata.version_and_kind = 0xFF;
    EXPECT_THROW(static_cast<void>(run.append_batch(oversized, this->io_buffer_)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(run.append_batch(invalid, this->io_buffer_)), std::invalid_argument);
    EXPECT_EQ(run.size(), original.size());
    EXPECT_EQ(TestFixture::read_file(path), before);
    TestFixture::expect_entries(this->collect(run), original);
    EXPECT_NO_THROW(static_cast<void>(run.append_batch(TestFixture::make_entries(1, 2), this->io_buffer_)));
    EXPECT_EQ(run.size(), 3U);
}

/** @brief Verifies exclusive create preserves an existing file's contents. */
TYPED_TEST(RunTest, ExistingFileIsNeverOverwrittenOrRemoved) {
    using RunT = typename TestFixture::RunT;
    const auto path = this->file_path();
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing-data-must-survive";
        ASSERT_TRUE(output.good());
    }
    const auto before = TestFixture::read_file(path);
    EXPECT_THROW((void)RunT::create(path,
        RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries}),
        std::system_error);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(TestFixture::read_file(path), before);
}

/** @brief Checks a real short block read permanently fails the run but still permits cleanup. */
TYPED_TEST(RunTest, TruncatedFileEntersFailedState) {
    using RunT = typename TestFixture::RunT;
    const auto path = this->file_path();
    auto run = RunT::create(path,
        RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries});
    static_cast<void>(run.append_batch(TestFixture::make_entries(3), this->io_buffer_));
    std::filesystem::resize_file(path, TestFixture::BlockBytes / 2);
    try {
        static_cast<void>(this->collect(run));
        FAIL() << "truncated block was accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("short block read"), std::string::npos);
    }
    this->expect_unusable(run);
    EXPECT_NO_THROW(run.clear());
    EXPECT_FALSE(std::filesystem::exists(path));
}

/** @brief Checks an unsupported on-disk entry kind is rejected during scan and marks the run failed. */
TYPED_TEST(RunTest, CorruptKindEntersFailedState) {
    using RunT = typename TestFixture::RunT;
    const auto path = this->file_path();
    auto run = RunT::create(path,
        RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries});
    static_cast<void>(run.append_batch(TestFixture::make_entries(3), this->io_buffer_));
    {
        std::fstream output(path, std::ios::in | std::ios::out | std::ios::binary);
        const std::uint64_t corrupt_word = 0xFF;
        output.seekp(3 * sizeof(std::uint64_t));
        output.write(reinterpret_cast<const char*>(&corrupt_word), sizeof(corrupt_word));
        ASSERT_TRUE(output.good());
    }
    EXPECT_THROW((void)this->collect(run), std::invalid_argument);
    this->expect_unusable(run);
    run.clear();
    EXPECT_FALSE(std::filesystem::exists(path));
}

/** @brief Injects an actual EFBIG write error without mocks and verifies failed-state cleanup. */
TYPED_TEST(RunTest, WriteFailureEntersFailedState) {
    using RunT = typename TestFixture::RunT;
    const auto path = this->file_path();
    auto run = RunT::create(path,
        RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries});
    const auto entries = TestFixture::make_entries(3);
    {
        const ScopedWriteFailure failure;
        try {
            static_cast<void>(run.append_batch(entries, this->io_buffer_));
            FAIL() << "write unexpectedly succeeded with a zero file-size limit";
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::make_error_code(std::errc::file_too_large));
        }
    }
    this->expect_unusable(run);
    EXPECT_EQ(std::filesystem::file_size(path), 0U);
    run.clear();
    EXPECT_FALSE(std::filesystem::exists(path));
}

/** @brief Verifies move construction/assignment transfer ownership and destruction removes the owned file. */
TYPED_TEST(RunTest, MoveAndDestructionTransferAndReleaseFiles) {
    using RunT = typename TestFixture::RunT;
    const auto source_path = this->file_path("source");
    const auto target_path = this->file_path("target");
    const auto entries = TestFixture::make_entries(3);
    {
        auto source = RunT::create(source_path,
            RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries});
        static_cast<void>(source.append_batch(entries, this->io_buffer_));
        auto moved = std::move(source);
        this->expect_unusable(source);
        EXPECT_NO_THROW(source.clear());
        EXPECT_TRUE(std::filesystem::exists(source_path));
        auto target = RunT::create(target_path,
            RunOptions{.block_bytes = TestFixture::BlockBytes, .max_entries = TestFixture::MaxEntries});
        static_cast<void>(target.append_batch(TestFixture::make_entries(1), this->io_buffer_));
        target = std::move(moved);
        EXPECT_FALSE(std::filesystem::exists(target_path));
        this->expect_unusable(moved);
        TestFixture::expect_entries(this->collect(target), entries);
    }
    EXPECT_FALSE(std::filesystem::exists(source_path));
    EXPECT_FALSE(std::filesystem::exists(target_path));
}

}  // namespace
}  // namespace cracking_lsm::test
