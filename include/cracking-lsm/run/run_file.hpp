// Copyright 2026 Weitang Ye
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <emds-toolkit/common/byte_view.hpp>
#include <emds-toolkit/common/requires.hpp>
#include <emds-toolkit/io/direct_io_buffer.hpp>

#include <cracking-lsm/run/block/block_format.hpp>
#include <cracking-lsm/run/block/block_view.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm::detail {

/**
 * @brief Block storage for an exclusively owned building Run working file.
 *
 * Entries remain in physical append order, including duplicate InternalKeys. Only one aligned
 * block buffer is retained in memory. This type owns temporary files, not published sealed Runs.
 */
template <PhysicalKey KeyT, bool KeyOnly = false>
class RunFile {
public:
    /** @brief Non-owning read-only view over loaded block bytes. */
    using ByteViewT = emds::common::ByteView;

    /** @brief Non-owning writable view over the reusable block buffer. */
    using MutBytesViewT = emds::common::MutBytesViewT;
    using EntryT = KVEntry<KeyT, KeyOnly>;
    using FormatT = BlockFormat<KeyT, KeyOnly>;

    /**
     * @brief Exclusively creates an empty working file and one aligned block buffer.
     *
     * @throws std::invalid_argument If the path or block size cannot represent a usable data block.
     * @throws std::system_error If the file or aligned buffer cannot be created.
     */
    [[nodiscard]] static auto create(std::filesystem::path file_path, std::size_t block_bytes) -> RunFile {
        emds::common::require_argument(!file_path.empty(),
            "RunFile requires a non-empty working-file path");
        emds::common::require_argument(block_bytes % emds::io::DirectIOBuffer::AlignBytes == 0,
            "RunFile block size must satisfy Direct I/O alignment");
        const auto block_capacity = FormatT::max_entries(block_bytes);
        emds::common::require_argument(block_capacity != 0,
            "RunFile block cannot hold one entry");

        std::optional<emds::io::DirectIOBuffer> io_buffer;
        bool created_working_file = false;
        try {
            io_buffer.emplace(emds::io::DirectIOBuffer::make(
                file_path, block_bytes, emds::io::FileOpenMode::create_new));
            created_working_file = true;
            auto state = std::make_unique<State>(
                file_path, block_bytes, block_capacity, std::move(*io_buffer));
            io_buffer.reset();
            return RunFile{std::move(state)};
        } catch (...) {
            io_buffer.reset();
            if (created_working_file) {
                std::error_code ignored_error;
                std::filesystem::remove(file_path, ignored_error);
            }
            throw;
        }
    }

    RunFile(const RunFile&) = delete;
    auto operator=(const RunFile&) -> RunFile& = delete;
    RunFile(RunFile&&) noexcept = default;
    auto operator=(RunFile&&) noexcept -> RunFile& = default;
    ~RunFile() = default;

    /**
     * @brief Appends at most one block-capacity batch in physical input order.
     *
     * A partial tail block is read, extended by BlockFormat, and rewritten as a whole block. A batch
     * that fills the tail may continue into one newly appended block. Duplicate entries are stored
     * and counted without comparing keys or scanning existing data for duplicates.
     *
     * @throws std::invalid_argument If the batch exceeds block_capacity or has an invalid kind.
     * @throws std::logic_error If the working file has been moved, cleared, or failed.
     * @throws std::system_error If Direct I/O fails.
     * @throws std::runtime_error If Direct I/O completes only a short block transfer.
     */
    auto append(std::span<const EntryT> entries) -> void {
        auto& state = require_usable();
        emds::common::require_argument(entries.size() <= state.block_capacity,
            "RunFile batch exceeds one data-block capacity");
        emds::common::require_argument(
            entries.size() <= std::numeric_limits<std::size_t>::max() - state.num_entries,
            "RunFile entry count overflows size_t");
        validate_entries(entries);
        if (entries.empty()) {
            return;
        }

        std::size_t consumed = 0;
        const auto tail_entries = state.num_entries % state.block_capacity;
        if (tail_entries != 0) {
            const auto tail_block_id = state.num_entries / state.block_capacity;
            read_block(state, tail_block_id);
            const auto available_entries = state.block_capacity - tail_entries;
            const auto appended_entries = std::min(available_entries, entries.size());
            const auto tail_input = entries.subspan(0, appended_entries);
            try {
                const auto descriptor = FormatT::describe_block(tail_entries, state.block_bytes);
                static_cast<void>(FormatT::append_block(
                    tail_input, writable_block(state), descriptor));
            } catch (...) {
                state.failed = true;
                throw;
            }
            write_block(state, tail_block_id);
            state.num_entries += appended_entries;
            consumed += appended_entries;
        }

        if (consumed != entries.size()) {
            const auto block_id = state.num_entries / state.block_capacity;
            const auto remaining_entries = entries.subspan(consumed);
            static_cast<void>(FormatT::encode_block(remaining_entries, writable_block(state)));
            write_block(state, block_id);
            state.num_entries += remaining_entries.size();
        }
    }

    /** @brief Visits every entry in physical block and append order. */
    template <typename VisitorT>
    requires std::invocable<VisitorT&, const EntryT&>
    auto scan(VisitorT&& visitor) const -> void {
        auto& state = require_usable();
        const auto num_blocks = block_count(state);
        for (std::size_t block_id = 0; block_id < num_blocks; ++block_id) {
            read_block(state, block_id);
            const auto loaded_block_view = block_view(state, block_id);
            for (std::size_t i = 0; i < loaded_block_view.size(); ++i) {
                const auto entry = decode_entry(state, loaded_block_view, i);
                std::invoke(visitor, entry);
            }
        }
    }

    /** @brief Returns the number of physical entries, including duplicates, in the working file. */
    [[nodiscard]] auto size() const -> std::size_t {
        return require_usable().num_entries;
    }

    /** @brief Reports whether the working file contains no entries. */
    [[nodiscard]] auto empty() const -> bool {
        return size() == 0;
    }

    /** @brief Returns the conservative fixed-width entry capacity of one data block. */
    [[nodiscard]] auto block_capacity() const -> std::size_t {
        return require_usable().block_capacity;
    }

    /**
     * @brief Closes and deletes the owned working file, permanently emptying this object.
     *
     * Calling clear on a moved-from or already cleared object does nothing.
     *
     * @throws std::system_error If removal of the closed working file fails.
     */
    auto clear() -> void {
        if (!state_) { return; }

        const auto file_path = state_->file_path;
        state_->io_buffer.reset();

        std::error_code remove_error;
        std::filesystem::remove(file_path, remove_error);
        if (remove_error) {
            state_->failed = true;
            throw std::system_error(remove_error, "failed to remove Run working file");
        }
        state_->owns_file = false;
        state_.reset();
    }

private:
    struct State {
        State(
            std::filesystem::path file_path_value,
            std::size_t block_bytes_value,
            std::size_t block_capacity_value,
            emds::io::DirectIOBuffer io_buffer_value
        )
            : file_path(std::move(file_path_value)),
              block_bytes(block_bytes_value),
              block_capacity(block_capacity_value),
              io_buffer(std::move(io_buffer_value)) {}

        ~State() noexcept {
            io_buffer.reset();
            if (owns_file) {
                std::error_code ignored_error;
                std::filesystem::remove(file_path, ignored_error);
            }
        }

        std::filesystem::path file_path;
        std::size_t block_bytes;
        std::size_t block_capacity;
        std::size_t num_entries = 0;
        std::optional<emds::io::DirectIOBuffer> io_buffer;
        bool failed = false;
        bool owns_file = true;
    };

    explicit RunFile(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    [[nodiscard]] auto require_usable() const -> State& {
        if (!state_) [[unlikely]] {
            throw std::logic_error("operation on a moved or cleared RunFile");
        }
        if (state_->failed) [[unlikely]] {
            throw std::logic_error("operation on a failed RunFile");
        }
        return *state_;
    }

    static auto validate_entries(std::span<const EntryT> entries) -> void {
        for (const auto& entry : entries) {
            emds::common::require_argument(entry.metadata.is_valid_kind(),
                "RunFile cannot append an invalid entry kind");
        }
    }

    [[nodiscard]] static auto writable_block(State& state) noexcept -> MutBytesViewT {
        return {state.io_buffer->buf_addr(), state.block_bytes};
    }

    [[nodiscard]] static auto readable_block(const State& state) noexcept -> ByteViewT {
        return {state.io_buffer->buf_addr(), state.block_bytes};
    }

    [[nodiscard]] static auto block_count(const State& state) noexcept -> std::size_t {
        return state.num_entries / state.block_capacity +
            static_cast<std::size_t>(state.num_entries % state.block_capacity != 0);
    }

    [[nodiscard]] static auto expected_block_entries(
        const State& state, std::size_t block_id
    ) noexcept -> std::size_t {
        const auto num_blocks = block_count(state);
        if (block_id + 1 < num_blocks || state.num_entries % state.block_capacity == 0) {
            return state.block_capacity;
        }
        return state.num_entries % state.block_capacity;
    }

    [[nodiscard]] static auto file_offset(const State& state, std::size_t block_id)
        -> std::uint64_t {
        emds::common::require_argument(block_id <=
                std::numeric_limits<std::uint64_t>::max() / state.block_bytes,
            "RunFile file offset overflows uint64_t");
        return static_cast<std::uint64_t>(block_id) * state.block_bytes;
    }

    static auto read_block(State& state, std::size_t block_id) -> void {
        try {
            const auto bytes_read = state.io_buffer->read_at(
                file_offset(state, block_id), 0, state.block_bytes);
            if (bytes_read != state.block_bytes) {
                throw std::runtime_error("RunFile encountered a short block read");
            }
        } catch (...) {
            state.failed = true;
            throw;
        }
    }

    static auto write_block(State& state, std::size_t block_id) -> void {
        try {
            const auto bytes_written = state.io_buffer->write_at(
                file_offset(state, block_id), 0, state.block_bytes);
            if (bytes_written != state.block_bytes) {
                throw std::runtime_error("RunFile encountered a short block write");
            }
        } catch (...) {
            state.failed = true;
            throw;
        }
    }

    [[nodiscard]] static auto block_view(State& state, std::size_t block_id)
        -> BlockView<KeyT, KeyOnly> {
        try {
            const auto descriptor = FormatT::describe_block(
                expected_block_entries(state, block_id),
                state.block_bytes);
            return BlockView<KeyT, KeyOnly>::make(readable_block(state), descriptor);
        } catch (...) {
            state.failed = true;
            throw;
        }
    }

    [[nodiscard]] static auto decode_entry(
        State& state, const BlockView<KeyT, KeyOnly>& block_view, std::size_t i
    ) -> EntryT {
        try {
            return block_view.entry_at(i);
        } catch (...) {
            state.failed = true;
            throw;
        }
    }

    std::unique_ptr<State> state_;
};

}  // namespace cracking_lsm::detail
