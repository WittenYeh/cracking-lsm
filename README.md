<!-- Generated with pyfiglet 1.0.4: pyfiglet -f ansi_regular -w 160 -s "cracking-lsm". -->
<pre align="center">
 ██████ ██████   █████   ██████ ██   ██ ██ ███    ██  ██████        ██      ███████ ███    ███&#32;
██      ██   ██ ██   ██ ██      ██  ██  ██ ████   ██ ██             ██      ██      ████  ████&#32;
██      ██████  ███████ ██      █████   ██ ██ ██  ██ ██   ███ █████ ██      ███████ ██ ████ ██&#32;
██      ██   ██ ██   ██ ██      ██  ██  ██ ██  ██ ██ ██    ██       ██           ██ ██  ██  ██&#32;
 ██████ ██   ██ ██   ██  ██████ ██   ██ ██ ██   ████  ██████        ███████ ███████ ██      ██&#32;
</pre>

# cracking-lsm

[cracking-lsm](https://github.com/WittenYeh/cracking-lsm) is a C++ project for a concurrent memtable and file-resident runs in a tiered Log-Structured Merge-tree (LSM-tree).

The write path uses an independent oneTBB `concurrent_multiset` Memtable, with a `max_entries`
flush threshold based on stored physical key entries (including duplicates, versions, and tombstones). The planned root also owns a disk overflow log:
insertion spills existing entries to that log when memory is needed; a query encountering overflow
pushes the complete root buffer to children. The overflow protocol is planned, not implemented.

The build links `TBB::tbb` from the official oneTBB repository (`https://github.com/uxlfoundation/oneTBB.git`),
pinned to the v2023.1.0 baseline `3046c8b0c29df995980003ea24f4d78c80ec0c8d`, and emds-toolkit.
Initialize dependencies with `git submodule update --init --recursive`. Existing checkouts can update
the oneTBB remote with `git submodule sync -- third-party/oneTBB`. The project uses the unmodified
upstream implementation and its forward-only iterators; no oneTBB source extensions are planned.
Abseil remains introduced as a submodule but is no longer
built or linked by the main library. oneTBB tests, examples and its optional malloc library are disabled.

Run data resides in files. Memtable capacity and Run lifecycle are separate concepts. Typed entries,
comparators, codecs, and block views remain available for operations on caller-owned memory.
File appends borrow an aligned block buffer and preserve physical input order, including duplicates.

The shared `cracking_lsm::KVEntry<KeyT, KeyOnly>` type and its metadata, comparator, and key concepts
live in `include/cracking-lsm/kv_entry/`. Include `<cracking-lsm/kv_entry/kv_entry.hpp>` to use entries
independently of Run. `RunState` belongs to `run/run_state.hpp`; entries carry an optional external
payload reference and support versions, tombstones, and key-only mode.

Shared operation results live in `include/cracking-lsm/engine/op_result/`, with one header per
result: `insertion_result.hpp`, `lookup_result.hpp`, `successor_result.hpp`, and `append_result.hpp`.
Include the specific header, such as `<cracking-lsm/engine/op_result/insertion_result.hpp>`, as needed.
The abstract `OpResult` interface lives in `op_result.hpp`, identifies the operation, and has a
virtual destructor. `InsertionState` and `QueryState` have their own `insertion_state.hpp` and
`query_state.hpp` headers. `InsertionResult`,
`LookupResult<KeyT, KeyOnly>`, `SuccessorResult<KeyT, KeyOnly>`, and `AppendResult` derive from it
and can be returned by value. Query results own optional entry copies; public successor results
contain a visible value or no entry. These runtime result objects are separate from the persisted
entry format. The engine's planned `Table` interface will coordinate operations across sources.

The current implementation provides the entry and block formats and a two-layer building Run:
`run/run.hpp` implements file operations and public policies, while `run/run_impl.hpp` owns their state.
`Run<KeyT, KeyComparatorT, KeyOnly>` in `run/run.hpp` supports `create(path, options, comparator)`
with a positive `max_entries`, an aligned `block_bytes`, and capacity for at least one entry per block.
Configuration validation precedes file creation, and an existing path is never overwritten.
`append_batch(entries, io_buffer)` accepts at most one block-capacity batch in physical input order, including
duplicates. The entire batch that reaches or crosses `max_entries` is accepted, sets `seal_required`,
and returns an `AppendResult` with the actual total count and flag. Later appends throw `std::logic_error`,
including empty batches; an empty batch before the threshold performs no I/O and returns the current state.
The Run remains `RunState::building`; reaching the threshold does not sort, deduplicate, or seal it.

`size()`, `empty()`, `max_entries()`, `block_bytes()`, `block_capacity()`, `seal_required()`, and `state()`
observe a usable Run without file I/O. State checks and complete batch validation precede writes;
invalid input preserves existing file contents and counts. I/O failure can leave partial writes and
makes all normal operations reject access, including state observations. Only a complete successful
append updates the physical count and seal flag and produces an `AppendResult`.
`scan(io_buffer, visitor)` reads every physical entry in append order, including duplicates and tombstones,
using the borrowed aligned buffer. It does not apply snapshot visibility. Visitor exceptions propagate;
I/O and decoding errors mark the Run failed. Entry references are valid only during each visitor call.
`RunImpl` holds the path, configuration, copied comparator, block capacity, physical count,
`run_state`, seal flag, failure flag, and cleanup ownership. Its `run_file` member directly owns an
optional `emds::io::DirectIOFile`, which owns the descriptor without an I/O buffer.
Run owns it through a `unique_ptr<RunImplT>`, so moves transfer ownership without invoking comparator operations.
Comparator copies may throw during creation, before any working file is opened; comparisons remain `noexcept`.
`clear()` closes and removes the temporary working file, and destruction also attempts cleanup.
Moving transfers cleanup responsibility; failed, moved-from, or cleared Runs permit cleanup and reassignment.
Run stores no buffer, index, or array that grows with the entry count. Each nonempty append or scan
requires a caller-owned `emds::io::DirectIOBuffer` with at least `block_bytes()` bytes; only the first
block-sized region is used. An empty or undersized required buffer is rejected before I/O without
marking the Run failed. Empty appends and empty scans do not use the buffer.
Creation, append, scan, validation, and explicit cleanup live in Run. RunImpl initializes state and
releases its owned resources on destruction; there is no separate RunFile class or nested State owner.

Allocate a buffer once with `DirectIOBuffer::make(capacity_bytes)` and reuse it across Runs after each
operation completes. Opening, moving, or clearing a Run does not allocate or release this memory.
During an operation, keep the buffer alive and exclusive; a scan visitor must not lend it to another
Run, overwrite it, or move it. Input entries must not overlap the buffer region used by append.
This is reusable I/O workspace, with no block cache or buffer pool. For example:

```cpp
#include <array>
#include <cstdint>
#include <cracking-lsm/run/run.hpp>

void append_to_two_runs() {
    using RunT = cracking_lsm::Run<std::uint64_t>;
    auto io_buffer = RunT::DirectIOBufferT::make(4096);
    const cracking_lsm::RunOptions options{.block_bytes = 4096, .max_entries = 1024};
    auto first = RunT::create("run-a.bin", options);
    auto second = RunT::create("run-b.bin", options);
    const std::array entries{RunT::EntryT::make(1, 100, 1)};
    static_cast<void>(first.append_batch(entries, io_buffer));
    static_cast<void>(second.append_batch(entries, io_buffer));
    first.scan(io_buffer, [](const RunT::EntryT& entry) { /* consume entry */ });
}
```

`Memtable<KeyT, KeyComparatorT, KeyOnly>` in `memtable/memtable.hpp` supports creation, insertion,
version-filtered point and successor queries, state observation, movement, and destruction.
`create` requires positive `max_entries`; an empty index has zero entries.
`size()`, `empty()`, `max_entries()`, and `flush_required()` expose its state.
Each insertion keeps one physical entry, including duplicates, historical versions and tombstones.

Memtable and Run share `EntryComparator`: keys follow the user comparator, while versions and kinds
remain descending within each key group. Memtable uses the official oneTBB public bound methods
and forward-only iterators directly; it has no reverse comparator adapter.

`lookup(key, read_version)` returns the newest eligible value or tombstone, or not_found.
`successor(key, read_version)` returns the least visible, undeleted key strictly after the bound in
user-comparator order. It excludes the entire equal-key group and skips groups whose newest
eligible entry is a tombstone, without exposing older values. The queries use a compound
`lower_bound` and a user-key `upper_bound`, respectively. Expected sequential costs are O(log N)
for point lookup and O(log N + E) for successor, where E is the number of entries examined.
Both use O(1) extra space, return owned copies, and remain available when the Memtable is full.

The strict successor satisfies `comp(query_key, candidate_key)` and excludes every entry in the
comparator-equivalent query group. Each candidate group is resolved to its newest visible version
before deciding whether to return it or skip the group. The former `predecessor` API,
`PredecessorResult` and `OpKind::predecessor` have been replaced by `successor`, `SuccessorResult`
and `OpKind::successor`. Run, StorageNode, LeafBucket and Table query plans adopt the same contract;
their file and global queries remain future work.

Choose the comparator when creating the index to select the desired **natural-key direction**:

| User comparator | Key order | `successor(25)` with visible, undeleted keys 10, 20, 30, 40 |
|---|---|---|
| `std::less<>` or a comparator returning `lhs < rhs` | Small keys first (ascending) | 30: natural-order successor |
| `std::greater<>` or a comparator returning `lhs > rhs` | Large keys first (descending) | 20: natural-order predecessor |

Thus users who need a natural-order predecessor select a descending comparator and call the
same successor API. It remains a successor relative to that comparator. This is an **ascending vs.
descending ordering** decision, not little-endian vs. big-endian byte order (sometimes called
"large endian"). Endianness describes byte representation and does not choose the query direction.
Custom comparators must obey the existing strict-weak-order, `noexcept`, non-throwing-copy and
concurrent-use requirements. All components of an index must use the same ordering and comparator
state; changing direction requires rebuilding the index. This API provides one direction per
configured index, not efficient queries in both natural-key directions on the same index.

Insertions, queries and observers can run concurrently. A query overlapping insertions is weakly
consistent: `read_version` is a data-version ceiling, not a stable MVCC snapshot or commit watermark.
For a stable view, the caller must exclude writes for the duration of the query. Comparators,
including their copies and any shared state, must support concurrent use. Move, assignment,
destruction and any future erase/spill require external exclusion of all operations on the object.
Run and the planned storage engine do not acquire thread safety through this Memtable change.

A successful insertion observes the completed physical entry count and sets a sticky atomic flush
flag when it reaches or exceeds `max_entries`. With sequential insertion and `max_entries = 1000`,
the first 1000 entries are accepted, the 1000th requests a flush, and the next is rejected.
Duplicates, historical versions and tombstones each consume one entry; this is not a distinct-key
limit. Later insertions that observe the flag return `memtable_full`; insertions already admitted
may still complete above the threshold. Invalid entries are still rejected when full.
`InsertionResult` contains `status`, `num_total_entries`, and `flush_required`; concurrent observations
need not describe a single instant. After writers finish, counts match the stored entries.
The flag reports that a flush is required; automatic spill and root transfer remain future work.

`MemtableImpl` owns the index, immutable configuration, insertion reservations, completed-entry
count and flush flag. Reservations protect against concurrent count overflow; the completed count
keeps `size()` O(1) instead of aggregating oneTBB's per-thread counters. The implementation remains
owned by `unique_ptr`, so moves transfer ownership without moving the comparator or index.

Memtable uses `concurrent_multiset`'s default `tbb_allocator<EntryT>`, with no custom allocator or
memory byte accounting. Entry alignment must not exceed `alignof(std::max_align_t)`; over-aligned
entry types are rejected at compile time. This restriction applies to Memtable, not the shared key concept.
Comparisons and comparator copies remain non-throwing. Preflight errors propagate before the
entry is modified; allocation and unexpected insertion failures report request context through
`utils/error.hpp` and terminate.

The final planned evaluation step (Step 43, after integration) will measure stored physical entry
count against the memory footprint of the in-memory data structure. It will publish raw samples,
plots and regression models for threshold selection, covering key size/alignment, both KeyOnly
modes, thread counts and input distributions. Separate measurements will distinguish live heap
allocations, allocator retention and process RSS; key-count thresholds do not promise a RAM limit.
A baseline model is `memory_bytes = intercept + bytes_per_entry * physical_entries`, with reported
fit error and a validated measurement range. Models and plots are planned, not yet measured, and
will provide user guidance without entering the runtime flush decision.

The earlier reverse-order predecessor implementation passed 64 focused tests with GCC 14.3.0 /
C++23 Debug, ASan, UBSan and leak detection: 42 Memtable, 6 comparator and 16 memory-accounting cases.
Its reverse-order queries were
checked against an unsorted reference model with custom orders, equivalent keys, versions and
tombstones in both KeyOnly modes. Concurrent insertion/query cases also passed; these checks do
not establish fixed MVCC snapshots, exhaustive concurrency coverage or performance bounds.
The successor migration updated the existing Memtable cases and moved the comparator cases to
`tests/entry_comparator_test.cpp`. The physical-entry threshold change also migrated the existing
capacity checks and retired the memory-accounting tests. No new test cases were added and the
changed code has not been compiled or tested. The old 64/64 result validates neither the successor
implementation nor the entry-count threshold; their verification remains Step 17.
Cross-source routing, root overflow handling, file queries, sealing and sorting remain later work.
