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
Memtable configuration, its template forward declaration, and the shared result types are also
defined. The B-tree memtable implementation, query routing, public `Run` lifecycle, queries, sorting,
and sealed B+Tree construction are planned in later implementation steps.
