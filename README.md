<!-- Generated with pyfiglet 1.0.4: pyfiglet -f ansi_regular -w 160 -s "cracking-lsm". -->
<pre align="center">
 ██████ ██████   █████   ██████ ██   ██ ██ ███    ██  ██████        ██      ███████ ███    ███&#32;
██      ██   ██ ██   ██ ██      ██  ██  ██ ████   ██ ██             ██      ██      ████  ████&#32;
██      ██████  ███████ ██      █████   ██ ██ ██  ██ ██   ███ █████ ██      ███████ ██ ████ ██&#32;
██      ██   ██ ██   ██ ██      ██  ██  ██ ██  ██ ██ ██    ██       ██           ██ ██  ██  ██&#32;
 ██████ ██   ██ ██   ██  ██████ ██   ██ ██ ██   ████  ██████        ███████ ███████ ██      ██&#32;
</pre>

# cracking-lsm

[cracking-lsm](https://github.com/WittenYeh/cracking-lsm) is a C++ project for a B-tree memtable and file-resident runs in a tiered Log-Structured Merge-tree (LSM-tree).

The planned write path inserts into an independent Abseil B-tree memtable, with a `memtable_bytes`
flush threshold based on live node allocation bytes. Point and predecessor queries consult the
memtable before storage and reconcile visible versions across sources. Memtable flush has a
reserved implementation step; its design is deferred.

Run data resides in files. Memtable capacity and Run lifecycle are separate concepts. Typed entries,
comparators, codecs, and block views remain available for operations on caller-owned memory.
File appends use one aligned block buffer and preserve physical input order, including duplicates.

The shared `cracking_lsm::KVEntry<KeyT, KeyOnly>` type and its metadata, comparator, and key concepts
live in `include/cracking-lsm/kv_entry/`. Include `<cracking-lsm/kv_entry/kv_entry.hpp>` to use entries
independently of Run. `RunState` belongs to `run/run_state.hpp`; entries carry an optional external
payload reference and support versions, tombstones, and key-only mode.

Shared operation results live in `include/cracking-lsm/engine/op_result/`, with one header per
result: `insertion_result.hpp`, `lookup_result.hpp`, `predecessor_result.hpp`, and `append_result.hpp`.
Include the specific header, such as `<cracking-lsm/engine/op_result/insertion_result.hpp>`, as needed.
The abstract `OpResult` interface lives in `op_result.hpp`, identifies the operation, and has a
virtual destructor. `InsertionState` and `QueryState` have their own `insertion_state.hpp` and
`query_state.hpp` headers. `InsertionResult`,
`LookupResult<KeyT, KeyOnly>`, `PredecessorResult<KeyT, KeyOnly>`, and `AppendResult` derive from it
and can be returned by value. Query results own optional entry copies; public predecessor results
contain a visible value or no entry. These runtime result objects are separate from the persisted
entry format. The engine's planned `Table` interface will coordinate operations across sources.

The current implementation provides the entry and block formats and the internal building-file
component `cracking_lsm::detail::RunFile<KeyT, KeyOnly>`, with Abseil already introduced as a dependency.
`Memtable<KeyT, KeyComparatorT, KeyOnly>` in `memtable/memtable.hpp` now supports creation, single-entry
insertion, snapshot point and predecessor queries, state observation, movement, and destruction.
`create` requires a positive `memtable_bytes`;
`size()`, `empty()`, `allocated_bytes()`, `memtable_bytes()`, and `flush_required()` expose its state and configuration.
`insert(entry)` validates the entry kind and physical count before copying an entry into the B-tree.
It retains duplicate entries, historical versions, and tombstones in the shared comparator order.
An insertion that reaches or crosses the live-byte threshold still returns `InsertionState::inserted`
and sets `flush_required=true`. Later valid insertions return `InsertionState::memtable_full` without
accepting the entry. Each `InsertionResult` reports the actual post-operation count, allocated bytes,
and flush flag. Invalid input is still rejected by validation when full; validation errors leave
existing data, allocation counts, and the flush flag unchanged. A positive threshold smaller than the
first node permits the first entry and then requires a flush; this step only reports that requirement.

`lookup(key, read_version)` returns the latest visible entry for a comparator-equivalent key as a
`LookupResult`: value, tombstone, or not_found. At equal versions, a tombstone takes precedence.
`predecessor(key, read_version)` returns a `PredecessorResult` containing the greatest visible,
undeleted key strictly before the bound in comparator order, or not_found. It resolves each key's
latest visible version before checking for deletion, so a tombstone never exposes that key's older
value. Both const queries default to `EntryMeta<KeyOnly>::MaxVersion`, reject larger read versions
before searching, and return owned entry copies in both payload-reference and key-only modes.
They remain available when full and preserve entry counts, allocated bytes, and the flush flag.
Point lookup uses a compound-key B-tree bound in O(log N); predecessor uses one user-key bound and
group traversal in O(log N + E), where E counts examined physical entries. Empty queries take O(1),
and each query uses O(1) extra space. Returned entries survive later insertion, movement, or destruction.

`MemtableImpl<KeyT, KeyComparatorT, KeyOnly>` in `memtable/memtable_impl.hpp` holds the configuration,
accounting object, B-tree types and storage, and flush flag. It lives in the `cracking_lsm` namespace;
its `validate_insertion(entry) const` checks the entry kind and the tree's actual count, and
`insert_entry(entry)` validates before inserting. It also implements entry lookup and predecessor
traversal. `predecessor_entry` directly scans preceding key groups and returns a value or no entry,
skipping groups whose latest visible entry is a tombstone. Planned Table reconciliation uses point
lookups across sources to check the visible version and deletion state of each possible predecessor.
`Memtable` privately owns the implementation through `impl_`, a `unique_ptr<ImplT>`, and handles
public interfaces, capacity policy, and operation results.
Moving a Memtable transfers its `unique_ptr`, preserves the accounting object's address, and carries
the flush flag with the data. Tree nodes are destroyed before the accounting object.
`memtable/memory_accounting.hpp` defines `cracking_lsm::MemoryAccounting` and
`cracking_lsm::CountingAllocator<T>`. The allocator wraps `std::allocator`, shares live
request-byte counts across copies and rebinds, and releases memory through the standard allocator.
It does not use an arena or enforce the memtable threshold during allocation.
Shared fatal-error reporting lives in `utils/error.hpp`; the allocator reports allocation errors with
context before termination. All user key comparators must be callable through a const reference,
with non-throwing invocation and conversion of the result to `bool`. `EntryComparator` directly
provides non-throwing comparisons, and Memtable additionally requires non-throwing comparator copies.
Memtable and Run default to `std::less<>`, whose exception specification follows the key's `<`
operation. Custom comparators should declare `operator()` as `const noexcept`. Violating a `noexcept`
promise invokes termination without the former Memtable comparison diagnostic wrapper.
Cross-source query routing, public `Run` lifecycle, sorting, and sealed B+Tree construction
are planned in later implementation steps.
