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

namespace cracking_lsm {

/** @brief Node-allocation byte threshold for an independent in-memory Memtable. */
struct MemtableOptions {
    /**
     * @brief Positive flush trigger in live B-tree allocation bytes; zero is unset.
     *
     * Counts node headers, entry slots, unused node capacity, and node alignment padding.
     * Fixed-size owner/counter objects, allocator bookkeeping, external payloads, file buffers,
     * and sorting memory are excluded.
     * Creation must reject zero. The threshold is checked after accepting a complete insertion,
     * which may exceed it; it is not a hard allocator limit.
     */
    std::size_t memtable_bytes = 0;
};

/** @brief Runtime block layout and physical-entry seal threshold for a file-resident Run. */
struct RunOptions {
    /** @brief Logical external-memory block size; it must satisfy Direct I/O alignment. */
    std::size_t block_bytes = 4096;

    /** @brief Positive physical-entry seal threshold; zero is unset and must be rejected by Run::create. */
    std::size_t max_entries = 0;
};

/** @brief Paths and sorting memory budget used while sealing a building Run. */
struct SealOptions {
    /** @brief Final path of the published sealed Run. */
    std::filesystem::path output_path;

    /** @brief Directory used for external-sort working files. */
    std::filesystem::path scratch_directory;

    /** @brief Maximum number of blocks available to the external sorter. */
    std::size_t memory_budget_blocks = 256;
};

}  // namespace cracking_lsm
