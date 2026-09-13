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

The current implementation provides the entry and block formats and the internal building-file
component `cracking_lsm::detail::RunFile<KeyT, KeyOnly>`, with Abseil already introduced as a dependency.
The memtable, query routing, public `Run` lifecycle, queries, sorting, and sealed B+Tree construction
are planned in later implementation steps.
