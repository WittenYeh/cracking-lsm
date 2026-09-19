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

#include <cstddef>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#include <emds-toolkit/io/direct_io_file.hpp>

#include <cracking-lsm/options.hpp>
#include <cracking-lsm/kv_entry/entry_comparator.hpp>
#include <cracking-lsm/kv_entry/key_concept.hpp>
#include <cracking-lsm/run/run_state.hpp>

namespace cracking_lsm {

/** @brief State and resource owner for Run; file operations and policies are implemented in run.hpp. */
template <PhysicalKey KeyT, KeyComparator<KeyT> KeyComparatorT, bool KeyOnly = false>
struct RunImpl {
    using EntryComparatorT = EntryComparator<KeyT, KeyComparatorT, KeyOnly>;
    using DirectIOFileT = emds::io::DirectIOFile;

    RunImpl(std::filesystem::path file_path_value, RunOptions options_value,
        std::size_t block_capacity_value, const KeyComparatorT& key_comparator)
        : file_path(std::move(file_path_value)), options(options_value), comparator(key_comparator),
          block_capacity(block_capacity_value) {}

    RunImpl(const RunImpl&) = delete;
    auto operator=(const RunImpl&) -> RunImpl& = delete;
    RunImpl(RunImpl&&) = delete;
    auto operator=(RunImpl&&) -> RunImpl& = delete;

    /** @brief Closes the file and best-effort removes the owned temporary path. */
    ~RunImpl() noexcept {
        run_file.reset();
        if (owns_file) {
            std::error_code ignored_error;
            std::filesystem::remove(file_path, ignored_error);
        }
    }

    /** @brief Working-file path used for exclusive creation and temporary-file cleanup. */
    std::filesystem::path file_path;

    /** @brief Runtime block size and positive physical-entry seal threshold. */
    RunOptions options;

    /** @brief Copied ordering for keys and descending version/kind within each key. */
    EntryComparatorT comparator;

    /** @brief Maximum physical entries per block for the configured key and metadata format. */
    std::size_t block_capacity;

    /** @brief Physical entry count after complete batches, including duplicates and tombstones. */
    std::size_t num_entries = 0;

    /** @brief Owned file descriptor without an I/O buffer; empty while the file is closed. */
    std::optional<DirectIOFileT> run_file;

    /** @brief Building/sealed lifecycle, independent of the seal threshold and failure flag. */
    RunState run_state = RunState::building;

    /** @brief Whether a successful batch reached max_entries and further appends must be rejected. */
    bool seal_required = false;

    /** @brief Whether an I/O, format, or cleanup failure has made normal operations unusable. */
    bool failed = false;

    /**
     * @brief Whether this owner is responsible for deleting the temporary working file.
     *
     * Set only after exclusive creation succeeds; retained when explicit removal fails.
     */
    bool owns_file = false;
};

}  // namespace cracking_lsm
