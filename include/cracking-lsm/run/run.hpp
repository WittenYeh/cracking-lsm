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
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <emds-toolkit/common/byte_view.hpp>
#include <emds-toolkit/common/requires.hpp>
#include <emds-toolkit/io/direct_io_buffer.hpp>
#include <emds-toolkit/io/direct_io_file.hpp>
#include <emds-toolkit/io/file_open_mode.hpp>

#include <cracking-lsm/options.hpp>
#include <cracking-lsm/run/block/block_descriptor.hpp>
#include <cracking-lsm/run/block/block_format.hpp>
#include <cracking-lsm/run/block/block_view.hpp>
#include <cracking-lsm/run/codec/key_codec/key_encoding_strategy.hpp>
#include <cracking-lsm/run/codec/key_codec/key_codec.hpp>
#include <cracking-lsm/run/codec/key_codec/native_key_codec.hpp>
#include <cracking-lsm/run/codec/meta_codec/meta_codec.hpp>
#include <cracking-lsm/run/codec/meta_codec/native_meta_codec.hpp>
#include <cracking-lsm/engine/op_result/append_result.hpp>
#include <cracking-lsm/engine/op_result/lookup_result.hpp>
#include <cracking-lsm/run/run_impl.hpp>
#include <cracking-lsm/run/run_state.hpp>
#include <cracking-lsm/kv_entry/entry_comparator.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/kv_entry/entry_meta.hpp>
#include <cracking-lsm/kv_entry/kv_entry.hpp>

namespace cracking_lsm {

/**
 * @brief Move-only building Run that owns a file and borrows an I/O buffer for each operation.
 *
 * Appends retain physical input order, duplicates, historical versions, and tombstones. The complete
 * batch that reaches the threshold is accepted; later appends are rejected.
 * A seal requirement does not change the building lifecycle or publish a sealed file.
 * Instances are used by one thread at a time. Failed, moved-from, or cleared objects reject normal
 * operations; they may still be cleared, destroyed, or assigned a new Run.
 * Buffers may be reused across Runs after each operation, but must remain alive and exclusive
 * throughout a call. Creating, moving, or clearing a Run does not allocate or release a buffer.
 */
template <PhysicalKey KeyT, KeyComparator<KeyT> KeyComparatorT = std::less<>, bool KeyOnly = false>
class Run {
public:
    using ByteViewT = emds::common::ByteView;
    using MutBytesViewT = emds::common::MutBytesViewT;
    using EntryT = KVEntry<KeyT, KeyOnly>;
    using BlockFormatT = BlockFormat<KeyT, KeyOnly>;
    using DirectIOBufferT = emds::io::DirectIOBuffer;

    /**
     * @brief Exclusively creates an empty working file with a positive physical-entry threshold.
     *
     * Run validates the working path, Direct I/O alignment, and nonzero block capacity before
     * opening the file. The comparator is copied before file creation; moving a Run never copies it.
     * @throws std::invalid_argument If max_entries is zero or the path/block configuration is invalid.
     * @throws std::system_error If the file cannot be created.
     * @throws std::bad_alloc If the implementation allocation fails.
     * Exceptions from comparator construction propagate without creating a working file.
     */
    [[nodiscard]] static auto create(std::filesystem::path file_path, RunOptions options,
        const KeyComparatorT& comparator = KeyComparatorT{}) -> Run {
        if (options.max_entries == 0) {
            throw std::invalid_argument("Run requires max_entries greater than zero");
        }
        emds::common::require_argument(!file_path.empty(), "Run requires a non-empty working-file path");
        emds::common::require_argument(
            options.block_bytes != 0 && options.block_bytes % DirectIOBufferT::AlignBytes == 0,
            "Run block size must be a positive multiple of Direct I/O alignment");
        const auto block_capacity = BlockFormatT::max_entries(options.block_bytes);
        emds::common::require_argument(block_capacity != 0, "Run block cannot hold one entry");

        auto run_impl = std::make_unique<RunImplT>(std::move(file_path), options, block_capacity, comparator);
        run_impl->run_file.emplace(emds::io::DirectIOFile::open(
            run_impl->file_path, emds::io::FileOpenMode::create_new));
        run_impl->owns_file = true;
        return Run{std::move(run_impl)};
    }

    Run(const Run&) = delete;
    auto operator=(const Run&) -> Run& = delete;
    Run(Run&&) noexcept = default;
    auto operator=(Run&&) noexcept -> Run& = default;
    ~Run() = default;

    /**
     * @brief Appends a complete batch of at most block_capacity() physical entries.
     *
     * An empty batch is a no-op only while appends are allowed. All input validation precedes I/O.
     * A successful batch may cross max_entries(); the resulting count and seal flag are returned.
     * A partial tail is read and rewritten, and remaining entries go into at most one new block.
     * Nonempty input borrows the first block_bytes() bytes of io_buffer. Input entries must not
     * overlap this region, and the caller must not move, release, or otherwise overwrite the buffer.
     * I/O failure can leave partial writes: the Run becomes unusable, and no result is returned.
     * @throws std::invalid_argument If the batch/kind/count or the required I/O buffer is invalid.
     * @throws std::logic_error If the Run is unusable, is not building, or already requires sealing.
     * @throws std::system_error If Direct I/O fails.
     * @throws std::runtime_error If Direct I/O completes only a short block transfer.
     */
    [[nodiscard]] auto append_batch(std::span<const EntryT> entries, DirectIOBufferT& io_buffer)
        -> AppendResult {
        auto& run_impl = require_usable();
        if (run_impl.run_state != RunState::building) {
            throw std::logic_error("Run append requires the building state");
        }
        if (run_impl.seal_required) {
            throw std::logic_error("Run requires sealing before further appends");
        }

        emds::common::require_argument(entries.size() <= run_impl.block_capacity,
            "Run batch exceeds one data-block capacity");
        emds::common::require_argument(
            entries.size() <= std::numeric_limits<std::size_t>::max() - run_impl.num_entries,
            "Run entry count overflows size_t");
        validate_entries(entries);
        if (entries.empty()) {
            return AppendResult{run_impl.num_entries, run_impl.seal_required};
        }
        validate_io_buffer(run_impl, io_buffer);

        try {
            std::size_t consumed = 0;
            const auto tail_entries = run_impl.num_entries % run_impl.block_capacity;
            if (tail_entries != 0) {
                const auto tail_block_id = run_impl.num_entries / run_impl.block_capacity;
                read_block(run_impl, tail_block_id, io_buffer);
                const auto available_entries = run_impl.block_capacity - tail_entries;
                consumed = std::min(available_entries, entries.size());
                const auto descriptor = BlockFormatT::describe_block(
                    tail_entries, run_impl.options.block_bytes);
                static_cast<void>(BlockFormatT::append_block(
                    entries.first(consumed), writable_block(run_impl, io_buffer), descriptor));
                write_block(run_impl, tail_block_id, io_buffer);
            }

            if (consumed != entries.size()) {
                const auto block_id = (run_impl.num_entries + consumed) / run_impl.block_capacity;
                static_cast<void>(BlockFormatT::encode_block(
                    entries.subspan(consumed), writable_block(run_impl, io_buffer)));
                write_block(run_impl, block_id, io_buffer);
            }
        } catch (...) {
            run_impl.failed = true;
            throw;
        }

        run_impl.num_entries += entries.size();
        run_impl.seal_required = run_impl.num_entries >= run_impl.options.max_entries;
        return AppendResult{run_impl.num_entries, run_impl.seal_required};
    }

    /**
     * @brief Visits every physical entry in append order using a caller-owned block buffer.
     *
     * The visitor borrows an entry for that invocation; it must not reenter I/O or modify/clear this Run.
     * It must not overwrite, move, release, or lend io_buffer to another operation, including another Run.
     * A nonempty scan requires at least block_bytes() bytes; an empty scan does not use the buffer.
     * Visitor exceptions propagate without marking the Run failed. I/O and decoding errors do fail it.
     * This raw scan preserves duplicates and tombstones and does not apply snapshot visibility.
     * @throws std::invalid_argument If a required buffer is empty, too small, or misaligned.
     */
    template <typename VisitorT>
    requires std::invocable<VisitorT&, const EntryT&>
    auto scan(DirectIOBufferT& io_buffer, VisitorT&& visitor) const -> void {
        auto& run_impl = require_usable();
        const auto num_blocks = block_count(run_impl);
        if (num_blocks == 0) { return; }
        validate_io_buffer(run_impl, io_buffer);
        for (std::size_t block_id = 0; block_id < num_blocks; ++block_id) {
            read_block(run_impl, block_id, io_buffer);
            const auto loaded_block_view = block_view(run_impl, block_id, io_buffer);
            for (std::size_t i = 0; i < loaded_block_view.size(); ++i) {
                const auto entry = decode_entry(run_impl, loaded_block_view, i);
                std::invoke(visitor, entry);
            }
        }
    }

    /** @brief Returns the physical entry count, including duplicates and tombstones. */
    [[nodiscard]] auto size() const -> std::size_t {
        return require_usable().num_entries;
    }

    /** @brief Reports whether the working file contains no entries. */
    [[nodiscard]] auto empty() const -> bool {
        return size() == 0;
    }

    /** @brief Returns the configured positive physical-entry seal threshold. */
    [[nodiscard]] auto max_entries() const -> std::size_t {
        return require_usable().options.max_entries;
    }

    /** @brief Returns the configured aligned data-block size in bytes. */
    [[nodiscard]] auto block_bytes() const -> std::size_t {
        return require_usable().options.block_bytes;
    }

    /** @brief Returns the fixed-width entry capacity of one data block. */
    [[nodiscard]] auto block_capacity() const -> std::size_t {
        return require_usable().block_capacity;
    }

    /** @brief Reports whether a successful batch reached the threshold and blocked further appends. */
    [[nodiscard]] auto seal_required() const -> bool {
        return require_usable().seal_required;
    }

    /** @brief Returns the lifecycle; reaching the append threshold still leaves the Run building. */
    [[nodiscard]] auto state() const -> RunState {
        return require_usable().run_state;
    }

    /**
     * @brief Closes and removes the owned working file, also permitting cleanup after failure.
     *
     * Repeated calls and calls on moved-from objects do nothing. After successful cleanup, normal
     * operations are rejected. Destruction also attempts to remove any remaining owned working file.
     * @throws std::system_error If file removal fails; ownership is retained for a later cleanup attempt.
     */
    auto clear() -> void {
        if (!run_impl_) { return; }
        run_impl_->run_file.reset();

        std::error_code remove_error;
        std::filesystem::remove(run_impl_->file_path, remove_error);
        if (remove_error) {
            run_impl_->failed = true;
            throw std::system_error(remove_error, "failed to remove Run working file");
        }
        run_impl_->owns_file = false;
        run_impl_.reset();
    }

private:
    using RunImplT = RunImpl<KeyT, KeyComparatorT, KeyOnly>;

    explicit Run(std::unique_ptr<RunImplT> run_impl) noexcept : run_impl_(std::move(run_impl)) {}

    [[nodiscard]] auto require_usable() const -> RunImplT& {
        if (!run_impl_) [[unlikely]] {
            throw std::logic_error("operation on a moved or cleared Run");
        }
        if (run_impl_->failed) [[unlikely]] {
            throw std::logic_error("operation on a failed Run");
        }
        return *run_impl_;
    }

    static auto validate_entries(std::span<const EntryT> entries) -> void {
        for (const auto& entry : entries) {
            emds::common::require_argument(entry.metadata.is_valid_kind(),
                "Run cannot append an invalid entry kind");
        }
    }

    /** @brief Rejects an unusable borrowed buffer before any file I/O or failed-state transition. */
    static auto validate_io_buffer(const RunImplT& run_impl, const DirectIOBufferT& io_buffer) -> void {
        emds::common::require_argument(io_buffer.buf_addr() != nullptr,
            "Run requires a non-empty I/O buffer");
        emds::common::require_argument(io_buffer.capacity() >= run_impl.options.block_bytes,
            "Run I/O buffer is smaller than block_bytes");
        emds::common::require_argument(
            reinterpret_cast<std::uintptr_t>(io_buffer.buf_addr()) % DirectIOBufferT::AlignBytes == 0,
            "Run I/O buffer address must be aligned to 4096 bytes");
    }

    [[nodiscard]] static auto writable_block(const RunImplT& run_impl, DirectIOBufferT& io_buffer) noexcept
        -> MutBytesViewT {
        return {io_buffer.buf_addr(), run_impl.options.block_bytes};
    }

    [[nodiscard]] static auto readable_block(const RunImplT& run_impl, const DirectIOBufferT& io_buffer)
        noexcept -> ByteViewT {
        return {io_buffer.buf_addr(), run_impl.options.block_bytes};
    }

    [[nodiscard]] static auto block_count(const RunImplT& run_impl) noexcept -> std::size_t {
        return run_impl.num_entries / run_impl.block_capacity +
            static_cast<std::size_t>(run_impl.num_entries % run_impl.block_capacity != 0);
    }

    [[nodiscard]] static auto expected_block_entries(const RunImplT& run_impl, std::size_t block_id) noexcept
        -> std::size_t {
        const auto num_blocks = block_count(run_impl);
        if (block_id + 1 < num_blocks || run_impl.num_entries % run_impl.block_capacity == 0) {
            return run_impl.block_capacity;
        }
        return run_impl.num_entries % run_impl.block_capacity;
    }

    [[nodiscard]] static auto file_offset(const RunImplT& run_impl, std::size_t block_id) -> std::uint64_t {
        emds::common::require_argument(
            block_id <= std::numeric_limits<std::uint64_t>::max() / run_impl.options.block_bytes,
            "Run file offset overflows uint64_t");
        return static_cast<std::uint64_t>(block_id) * run_impl.options.block_bytes;
    }

    static auto read_block(RunImplT& run_impl, std::size_t block_id, DirectIOBufferT& io_buffer) -> void {
        try {
            const auto bytes_read = run_impl.run_file->read_at(
                file_offset(run_impl, block_id), io_buffer, 0, run_impl.options.block_bytes);
            if (bytes_read != run_impl.options.block_bytes) {
                throw std::runtime_error("Run encountered a short block read");
            }
        } catch (...) {
            run_impl.failed = true;
            throw;
        }
    }

    static auto write_block(RunImplT& run_impl, std::size_t block_id, const DirectIOBufferT& io_buffer)
        -> void {
        try {
            const auto bytes_written = run_impl.run_file->write_at(
                file_offset(run_impl, block_id), io_buffer, 0, run_impl.options.block_bytes);
            if (bytes_written != run_impl.options.block_bytes) {
                throw std::runtime_error("Run encountered a short block write");
            }
        } catch (...) {
            run_impl.failed = true;
            throw;
        }
    }

    [[nodiscard]] static auto block_view(RunImplT& run_impl, std::size_t block_id,
        const DirectIOBufferT& io_buffer) -> BlockView<KeyT, KeyOnly> {
        try {
            const auto descriptor = BlockFormatT::describe_block(
                expected_block_entries(run_impl, block_id), run_impl.options.block_bytes);
            return BlockView<KeyT, KeyOnly>::make(readable_block(run_impl, io_buffer), descriptor);
        } catch (...) {
            run_impl.failed = true;
            throw;
        }
    }

    [[nodiscard]] static auto decode_entry(RunImplT& run_impl, const BlockView<KeyT, KeyOnly>& block_view,
        std::size_t i) -> EntryT {
        try {
            return block_view.entry_at(i);
        } catch (...) {
            run_impl.failed = true;
            throw;
        }
    }

    std::unique_ptr<RunImplT> run_impl_;
};

}  // namespace cracking_lsm
