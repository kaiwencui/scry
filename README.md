# scry

**Sub-millisecond local code search engine in pure C++23.**

`scry` combines `ripgrep`'s CLI ergonomics and `.gitignore` awareness with a zero-copy `mmap` 32 KiB chunked trigram index, SIMD-compressed posting lists, an LSM incremental delta overlay, and a quiet resident `inotify`/`FSEvents` daemon — packed into a single **603 KB** stripped binary (~10.6k SLOC in `src/`, **zero external runtime dependencies**: no Boost, no Protobuf, no `liburing`, no `ncurses`, no RE2/PCRE2).

Across **7 code search engines** benchmarked under identical conditions on a pinned checkout of **`torvalds/linux`** (`5f22fb204`, **97,371 tracked files, 1,598 MiB raw text**) — `scry`, `ripgrep (rg)`, `git grep`, `ugrep`, `ugrep --index`, `csearch` (Russ Cox), `Zoekt` (Sourcegraph), and `Livegrep` (Stripe) — `scry` sweeps **12 of 12 queries** (6/6 one-shot CLI and 6/6 resident daemon, all sub-1ms in daemon mode) while placing first on cold index build speed, incremental edit latency, and resident memory footprint:

| Metric (`torvalds/linux`, 97,371 files / 1.6 GB) | `scry` / `scryd` | Best Competitor | Advantage |
|---|---:|---:|---|
| **Cold full index build** | **4.36 s** (`366 MiB/s`) | 8.62 s (`cindex`) / 40.1 s (`Livegrep`) / 110.4 s (`Zoekt`) | **2.0×–25.3× faster** |
| **On-disk index size** | **453 MiB (`0.28×`)** | 170 MiB (`cindex`) / 3,287 MiB (`Zoekt`) / 7,589 MiB (`Livegrep`) | **7.3× smaller than Zoekt, 16.8× smaller than Livegrep** |
| **Daemon Private Dirty RAM (rest → warm)** | **3.21 → 20.63 MiB** | 102.3 → 73.8 MiB (`Zoekt`) / 60.3 → 238.9 MiB (`Livegrep`) | **3.6×–32× lower private dirty RAM** |
| **Daemon Total RSS (rest → warm)** | **9.1 → 48.8 MiB** | 202.0 → 320.8 MiB (`Zoekt`) / 557.2 → 2,126.2 MiB (`Livegrep`) | **6.6×–43.6× lower warm RSS** |
| **One-shot CLI: rare literal (`update_curr_dl_se`)** | **1.23 ms** | 11.61 ms (`csearch`) / 149.8 ms (`rg`) / 533 ms (`git grep`) | **9.4× vs `csearch`, 122× vs `rg`** |
| **One-shot CLI: case-insensitive (`-i SCHED_FLAG_UTIL_CLAMP`)** | **1.64 ms** | 49.55 ms (`zoekt`) / 137.8 ms (`rg`) | **30× vs `zoekt` CLI, 84× vs `rg`** |
| **Daemon IPC: rare literal (`update_curr_dl_se`)** | **0.21 ms** | 0.58 ms (`Zoekt`) / 0.67 ms (`Livegrep`) | **2.8×–3.2× faster** |
| **Daemon IPC: medium literal (`sched_entity`, 641 hits)** | **0.45 ms** | 3.13 ms (`Zoekt`) / 7.26 ms (`Livegrep`) | **7.0×–16.1× faster** |
| **Daemon IPC: `-i SCHED_FLAG_UTIL_CLAMP` (22 hits)** | **0.08 ms** | 0.61 ms (`Zoekt`) / 0.76 ms (`Livegrep`) | **7.6×–9.5× faster** |
| **Daemon IPC: complex regex (`struct\s+sched_[a-z_]+_entity`)** | **0.78 ms** | 4.83 ms (`Livegrep`) / 12.15 ms (`Zoekt`) | **6.2×–15.6× faster** |
| **Daemon IPC: header glob (`rq_clock_pelt` in `*.h`)** | **0.06 ms** | 0.58 ms (`Zoekt`) / 1.04 ms (`Livegrep`) | **9.7×–17.3× faster** |
| **Live incremental update (single-file edit / 2k checkout storm)** | **0.12 ms / 27.1 ms** | Full shard/index rebuild (`8–110 s`) | **Real-time LSM delta** |
| **Binary size (stripped C++23 LTO)** | **603 KB** (`695 KB` unstripped) | 1.2 MB (`ug`) / 3.3 MB (`csearch`) / 17–44 MB (`Livegrep`/`Zoekt`) | **Zero runtime dependencies** |

---

## 3 Surfaces, 1 Binary

1. **Instant CLI (`scry <pattern> [path ...]`)** — Drop-in `ripgrep`-style interface (`-i`, `-s`, `-S`, `-F`, `-w`, `-x`, `-v`, `-o`, `-e`, `-g`, `-t`/`-T`, `-C`/`-A`/`-B`, `-c`, `-l`, `-q`, `-0`, `--files`, `--files-without-match`, `--vimgrep`, `--heading`, `--json`, `-u`/`-uu`/`-uuu`, stdin piping, multiple paths, `--def`, `--boost-defs`). Automatically queries `scryd` over `AF_UNIX` if running, falls back to direct zero-copy `mmap` of `index.scry`, or falls back to a parallel AVX-512/AVX2/NEON linear scanner for unindexed repos and `<3`-char / `.*` queries.
2. **Zero-Lag Interactive Picker (`scry -I [query] [path]`)** — Raw `termios` split-pane terminal UI with streaming Top-K results, live file preview centered on the match line, editor jump (`--open` → `$EDITOR` / `zed {file}:{line}:{col}`), and atomic keystroke cancellation so typing never stalls the UI thread.
3. **Quiet Resident Indexer (`scry watch` / `scry daemon`)** — Watches registered repositories via Linux `inotify` or macOS `FSEvents`, applies file saves to an in-memory LSM delta overlay in `< 0.2 ms`, coalesces `git checkout`/`rebase` storms, swaps snapshots lock-free, persists watched repos in `~/.config/scry/repos.txt`, and serves sub-millisecond queries over a Unix domain socket.

---

## Benchmarks

### Methodology & Fairness Rules

All numbers below were captured on an **AMD Ryzen 9 8945HX** (16 cores / 32 threads, AVX-512, 64 MiB L3 across 2 CCDs, Ubuntu Linux 7.3) using the reproducible harness in [`scripts/bench_matrix.py`](scripts/bench_matrix.py):

- **Exact same repository & file set**: Pinned shallow clone of `torvalds/linux` at `5f22fb204` (`97,371` tracked files, `1,598.06 MiB` raw non-binary text). Every engine respects `.gitignore` and skips binary files.
- **Two modes reported transparently**: **Mode A (One-Shot CLI)** compares cold process invocations (`scry --no-daemon` vs `rg`, `git grep`, `ugrep`, `ugrep --index`, `csearch`, `zoekt` CLI); **Mode B (Resident Daemon)** compares resident hot-memory servers (`scryd` over `AF_UNIX` vs `zoekt-webserver` HTTP/JSON vs `livegrep` HTTP→gRPC), plus `scryd` invoked through a full CLI process spawn.
- **Strict correctness oracle**: Every benchmark round records exact match counts from all 7 engines side-by-side against `ripgrep 15.2.0`. All 6 queries produced **100% identical match counts** across every single engine (`9`, `641`, `22`, `214`, `20`, `285`).
- **Timing**: 1–2 warmup runs followed by 7 timed runs per query, reporting median (`p50`) and tail (`p99`). Daemon RSS and `Private_Dirty` are read directly from Linux `/proc/<pid>/smaps_rollup` both at rest and after all queries complete.

### 1. Index Build, On-Disk Size & Daemon Memory (`torvalds/linux`, 1,598 MiB)

| Engine | Language / Architecture | Build Time | Build Speed | Index Size (Ratio) | Daemon RSS (Rest → Warm) | Private Dirty (Rest → Warm) |
|---|---|---:|---:|---:|---:|---:|
| **`scry` / `scryd`** | C++23 (`mmap` 32KiB-chunk trigram + LSM) | **4.36 s** | **366.4 MiB/s** | **453.1 MiB (0.28×)** | **9.1 → 48.8 MiB** | **3.21 → 20.63 MiB** |
| **`csearch` (`cindex`)** | Go (Russ Cox trigram) | 8.62 s | 185.3 MiB/s | **170.2 MiB (0.11×)** | — (CLI only) | — |
| **`Livegrep`** | C++ (suffix array) | 40.13 s | 39.8 MiB/s | 7,589.4 MiB (4.75×) | 557.2 → 2,126.2 MiB | 60.34 → 238.88 MiB |
| **`ugrep --index`** | C++ (file-level Bloom index) | 80.42 s | 19.9 MiB/s | 435.8 MiB (0.27×) | — (CLI only) | — |
| **`Zoekt`** | Go (trigram shards + content) | 110.38 s | 14.5 MiB/s | 3,287.3 MiB (2.06×) | 202.0 → 320.8 MiB | 102.26 → 73.82 MiB |

### 2. Mode A — One-Shot CLI Latency (p50 ms, cold process per invocation — **6 / 6 wins**)

| Query (verified identical match count) | `scry` (cold CLI) | `csearch` | `zoekt` CLI | `ripgrep` (`rg`) | `git grep` | `ugrep` | `ugrep --index` |
|---|---:|---:|---:|---:|---:|---:|---:|
| **Q1**: Rare literal `update_curr_dl_se` (`9`) | **1.23 ms** | 11.61 ms | 51.70 ms | 149.83 ms | 533.06 ms | 734.43 ms | 887.33 ms |
| **Q2**: Medium literal `sched_entity` (`641`) | **3.72 ms** | 86.25 ms | 48.65 ms | 128.60 ms | 486.04 ms | 711.06 ms | 886.87 ms |
| **Q3**: Case-insensitive `-i SCHED_FLAG_UTIL_CLAMP` (`22`) | **1.64 ms** | 121.29 ms | 49.55 ms | 137.75 ms | 562.26 ms | 710.07 ms | 985.95 ms |
| **Q4**: Regex `struct\s+sched_[a-z_]+_entity` (`214`) | **5.35 ms** | 104.35 ms | 59.89 ms | 147.60 ms | 1,188.84 ms | 593.84 ms | 1,019.43 ms |
| **Q5**: Header glob `rq_clock_pelt` in `*.h` (`20`) | **1.13 ms** | 3.56 ms | 39.74 ms | 63.45 ms | 288.63 ms | 553.11 ms | 771.44 ms |
| **Q6**: Common literal `vruntime` (`285`) | **2.66 ms** | 7.73 ms | 54.32 ms | 135.87 ms | 504.99 ms | 700.94 ms | 861.98 ms |

### 3. Mode B — Resident Daemon / Hot-Memory Latency (p50 ms — **6 / 6 wins, all sub-1ms**)

| Query (verified identical match count) | `scryd` (AF_UNIX IPC) | `scryd` (CLI → AF_UNIX) | `Zoekt` (HTTP) | `Livegrep` (HTTP→gRPC) |
|---|---:|---:|---:|---:|
| **Q1**: Rare literal `update_curr_dl_se` (`9`) | **0.21 ms** | 1.16 ms | 0.58 ms | 0.67 ms |
| **Q2**: Medium literal `sched_entity` (`641`) | **0.45 ms** | 2.02 ms | 3.13 ms | 7.26 ms |
| **Q3**: Case-insensitive `-i SCHED_FLAG_UTIL_CLAMP` (`22`) | **0.08 ms** | 1.14 ms | 0.61 ms | 0.76 ms |
| **Q4**: Regex `struct\s+sched_[a-z_]+_entity` (`214`) | **0.78 ms** | 1.74 ms | 12.15 ms | 4.83 ms |
| **Q5**: Header glob `rq_clock_pelt` in `*.h` (`20`) | **0.06 ms** | 1.28 ms | 0.58 ms | 1.04 ms |
| **Q6**: Common literal `vruntime` (`285`) | **0.15 ms** | 1.69 ms | 1.67 ms | 5.30 ms |

### 4. Incremental Updates & Engine Microbenchmarks (`./scry-bench`)

| Metric | Measured Result |
|---|---|
| Single-file save → searchable (LSM delta) | **0.116 ms** |
| 2,000-file `git checkout` storm batch | **27.11 ms** |
| Symbol definition lookup (`--def`, p50) | **0.007 ms** |
| Interactive Top-100 on `return` (p50) | **0.160 ms** |

### Honest Trade-offs & Caveats

- **Short (`< 3`-char) / trigram-less queries**: Like any trigram index (`Zoekt`, `csearch`), queries with no 3-byte literal requirement (e.g. `->`, `[0-9]{4}`) fall back to parallel AVX-512/NEON linear scanning (similar to `rg`). `Livegrep`'s 7.6 GB suffix array can still answer 1–2 char queries in milliseconds at the cost of 17× disk and 44× warm RAM.
- **Smallest disk footprint**: `cindex` (`170 MiB`, `0.11×`) is smaller than `scry` (`453 MiB`, `0.28×`) because `cindex` stores only exact-case document trigrams — no hybrid `-i` table, no 32 KiB chunks, no path trigram index, and no 3.87M-entry symbol table.
- **Daemon RPC transport**: `scryd` uses framed binary RPC over `AF_UNIX` (~0.05 ms overhead), whereas `Zoekt` and `Livegrep` serve loopback HTTP/JSON (~0.2–0.4 ms overhead). On Q2, Q4, Q5, and Q6, `scryd`'s 6×–35× margins come from the engine itself (even full CLI fork/exec through `scryd` beats `Zoekt` and `Livegrep` on Q2, Q4, and Q6).

---

## Technical Architecture

### 1. On-Disk v2 Zero-Copy `mmap` Index Format (`src/index_format.hpp`)

All sections are **64-byte aligned** for AVX-512 vector loads and direct `mmap(PROT_READ, MAP_PRIVATE)` with zero deserialization on startup. Updates write to `index.scry.tmp.<pid>` and publish via atomic `rename()`.

```text
Offset 0 (all sections 64-byte aligned)
┌─────────────────────────────────────────────────────────────────────────┐
│ IndexHeader (<= 320 B)                                                  │
│   magic="SCRYIDX\0", version=2, flags, num_docs, num_chunks,            │
│   git_index_mtime_ns, git_head_hash, section offsets, xxhash64 checksum │
├─────────────────────────────────────────────────────────────────────────┤
│ FileTable      : num_docs   × FileEntry  (40 B: path, lang, mtime,      │
│                  inode, first_chunk, chunk_count)                       │
│ ChunkTable     : num_chunks × ChunkEntry (16 B: file_id, byte_off,      │
│                  byte_len, start_line_no)                               │
│ PathArena      : contiguous NUL-terminated relative paths               │
├─────────────────────────────────────────────────────────────────────────┤
│ ExactTrigramIndex (ChunkId postings)                                    │
│   prefix_offsets[65537] (u32, 256 KiB) -> O(1) 2-byte prefix bucket     │
│   entries[]             (16 B TrigramEntry, sorted within each bucket)  │
│   postings_arena        (8-byte aligned Inline / BP128 / Bitset)        │
├─────────────────────────────────────────────────────────────────────────┤
│ HybridLowerTrigramIndex (ChunkId postings, ONLY trigrams w/ uppercase)  │
│   Same 2-level prefix directory + compressed postings arena             │
├─────────────────────────────────────────────────────────────────────────┤
│ PathTrigramIndex (FileId postings, lowercased relative paths)           │
├─────────────────────────────────────────────────────────────────────────┤
│ SymbolIndex                                                             │
│   entries[]    : num_symbols × SymbolEntry (16 B: lower_hash32,         │
│                  name_off, doc_id, (kind<<24 | line_no))                │
│   symbol_arena : string-interned NUL-terminated symbol names            │
└─────────────────────────────────────────────────────────────────────────┘
```

#### Bit-Level Struct Layouts (Cache-Line Friendly)

- **`TrigramEntry` (16 bytes — 4 per cache line)**:
  - `trigram_meta (u32)`: bits `[0..23]` = packed 24-bit trigram `(b0<<16 | b1<<8 | b2)`, bits `[24..25]` = `PostingEncoding` (`0=Inline`, `1=BP128`, `2=Bitset`), bit `[26]` = `has_upper_variants`.
  - `doc_freq (u32)`: exact cardinality for O(1) query-planner selectivity ordering.
  - `offset_or_inline (u64)`: for `doc_freq <= 2`, holds up to 2 `ChunkId`s **inline** (zero indirection); otherwise byte offset into `postings_arena`.
- **O(1) Prefix-Bucketed Lookup**: Indexing `prefix_offsets[t >> 8]` gives `[lo, hi)` of size `0..256` (typically 1–8 entries) with zero hash collisions and a tiny branchless binary search.

### 2. 32 KiB Line-Aligned Virtual Chunking (`src/indexer.cpp`, `src/searcher.cpp`)

Whole-file trigram indexes suffer badly on multi-megabyte generated files. On `torvalds/linux`, `Q3` (`-i SCHED_FLAG_UTIL_CLAMP`) initially took **7.49 ms** because 20 generated 6–7 MB AMD GPU register headers (`dcn_*_sh_mask.h`, ~140 MB total) contained all 19 trigrams scattered across thousands of unrelated `#define` lines.

`scry` solves this without storing positional byte offsets or bloating disk:
- Files `<= 32 KiB` (~90% of source files) stay a single chunk.
- Files `> 32 KiB` are split strictly on `\n` boundaries into ~32 KiB `ChunkEntry` slices recording `(file_id, byte_off, byte_len, start_line_no)`.
- On `torvalds/linux`, `96,781` files map to `127,896` chunks (only `+32%` universe size).
- Because chunks split on `\n`, no single-line match can straddle chunks. Verification coalesces adjacent matching chunks and `pread()`s **only** the matching 32 KiB slices (shifting reported line numbers by `start_line_no - 1`).
- **Measured impact on `torvalds/linux`**: `Q3` candidates dropped from `37` files (~140 MB) to `7` tiny slices, cutting `scryd` latency **93×** from `7.49 ms` → **`0.08 ms`**.

### 3. Adaptive 3-Tier Posting Lists (`src/posting.hpp`, `src/posting.cpp`)

| Tier | Condition | Layout | Intersection Strategy |
|---|---|---|---|
| **Tier 0: Inline** | `freq <= 2` | Packed directly inside `TrigramEntry::offset_or_inline` | O(1) inline check, zero arena read |
| **Tier 1: SIMD-BP128** | `2 < freq < N/16` | 128-id blocks: `(first_id, max_id, bit_width)` 12 B header + 4-lane word-striped packed deltas (`16 * bw` B) + LEB128 tail | O(1) block skipping (`if max_id < target`, jump `16 * bw` bytes without unpacking) + galloping merge |
| **Tier 2: Dense Bitset** | `freq >= N/16` | 8-byte aligned `(N+63)/64` `u64` words | O(`|candidates|`) single-bit tests; AVX-512/AVX2/NEON `bitset_and`/`bitset_or` |

### 4. Hybrid Lowercased Trigram Table (Sub-ms `-i` Without 8× Blowup)

A naive exact trigram index must union up to $2^3 = 8$ posting lists per trigram for `-i`, while a naive duplicate lower table doubles index size. `scry` builds a **hybrid lower table**:
- Partitioning by `FibonacciHash(lower_trigram(t))` guarantees all case variants (`Foo`, `FOO`, `fOo`, `foo`) land in the **exact same partition** on the same thread.
- If a lowercase trigram has **no uppercase variants** anywhere in the corpus, `has_upper_variants = 0` is set on its exact entry and **nothing** is written to `LowerTrigramIndex`; `-i` lookups fall back directly to the exact entry with zero duplication.
- Only trigrams with uppercase occurrences emit a merged union posting list into `LowerTrigramIndex` (`145,114` entries vs `553,212` exact entries on Linux).

### 5. Query Pipeline: Regex AST → OR-Factored Planner → Lock-Free Lazy DFA

```text
  pattern ──▶ Recursive-Descent Regex AST (src/regex_engine.cpp)
                    │
        ┌───────────┴──────────────────────────────┐
        ▼                                          ▼
  TrigramQuery Algebra                     SIMD Prefilter + NFA/DFA
  • ExactSet / Prefix / Suffix             • Pure literal -> AVX-512 2-byte filter
  • Common-atom factoring out of OR(...)   • Regex literal -> buffer-wide SIMD jump
  • O(1) doc_freq rarest-first ordering    • Line verify -> thread-local lock-free
  • Prune oversized OR unions in AND         Lazy DFA (1024 states) + Thompson NFA
                    │                               ▲
                    ▼                               │
  Base MmapIndex + LSM DeltaSegment ──▶ Coalesced 32 KiB pread / mmap
  (PathTrigramIndex FileId bitset)      via persistent WorkerPool (16 threads)
```

- **SIMD Verification (`src/simd.hpp`)**: Buffer-wide AVX-512/AVX2/NEON two-byte filter (`first_byte` + `last_byte` simultaneous compare, 64 B/iter on AVX-512) jumps straight to candidate offsets and computes 1-based line numbers via AVX-512 `\n` popcount — never splitting non-matching lines.
- **Lock-Free Lazy DFA**: Each compiled pattern gets a monotonic `pattern_id_`; worker threads lazily populate a `thread_local` 256-entry transition table (`0` mutex locks in the inner byte loop), guaranteeing linear $O(N)$ verification with zero catastrophic backtracking.
- **Why `mmap` >= 256 KiB, `pread` < 256 KiB**: Empirical A/B profiling on Linux showed `mmap` avoids page-cache copies on multi-MB files (`3.6×` faster than `pread` on 140 MB), while `pread` into a reusable buffer avoids VMA/TLB setup overhead on small files.

### 6. Parallel Indexing Pipeline (`src/gitignore.cpp`, `src/io_uring_reader.cpp`, `src/indexer.cpp`)

1. **Parallel Directory Walk**: Expands depth-0 and depth-1 directories into a work-stealing queue of `(rel_dir, GitignoreMatcher)` tasks across `WorkerPool`, cutting Linux walk time `6.3×` (`586 ms → 93 ms`).
2. **Raw Linux `io_uring` Ingestion**: Calls `__NR_io_uring_setup` and `__NR_io_uring_enter` directly (zero `liburing` dependency) to pipeline `OPENAT → READ → CLOSE` batches. Automatically budgets ring depth from `getrlimit(RLIMIT_NOFILE)` so concurrent rings never drop files on `-EMFILE`, with automatic `pread` fallback.
3. **2 MiB Dirty-Reset Bitset Dedup**: Each worker uses a reusable $2^{24}$-bit (2 MiB) bitset and resets only touched words (`seen_bits[t >> 6] = 0`) per chunk — O(unique trigrams), zero hash collisions, zero heap allocations.
4. **Stable Zero-Sort 3-Pass LSD Radix Merge**: Each worker assigns strictly ascending `local_chunk_id`s. Concatenating worker `0..W-1` buckets in thread order yields a globally `ChunkId`-sorted stream; a **stable** 3-pass 8-bit LSD radix sort on the 24-bit trigram produces 100% sorted posting lists for every trigram run with **zero per-run `std::sort`**.

### 7. Incremental LSM Delta Engine & Freshness Guarantee (`src/delta.cpp`, `src/daemon.cpp`)

- **Lock-free Snapshot Swap**: `LiveIndex` publishes immutable `IndexSnapshot { base: shared_ptr<const MmapIndex>, delta: shared_ptr<const DeltaSegment> }` via `std::atomic<std::shared_ptr<const IndexSnapshot>>`. Readers never wait on writers.
- **Tombstones + Delta Postings**: Modified or deleted base files set a single bit in `base_tombstones` (checked in O(1) per candidate); added/edited files live in `DeltaSegment` (extracted via an L1-cache-resident flat hash table in `~0.12 ms` per save, parallelized for multi-hundred-file checkout storms, and auto-compacted when delta exceeds 2,000 files or 10% of base).
- **Zero-Staleness Without Daemon**: On repos `<= 5,000` files, daemon-less CLI queries run a fast `lstat` pass (`< 0.5 ms`) to patch unstaged edits automatically. On huge monorepos (`> 5,000` files), CLI checks `.git/index` mtime + `HEAD` hash in ~5 µs and verifies every candidate against live disk (with `--fresh` or `scryd` providing full real-time coverage).

---

## Codebase Map

| File | Responsibility |
|---|---|
| `src/common.hpp` | Core types, `MmapFile` RAII, 64-bit hashing, timers, XDG cache/config/socket paths, `/proc` memory readers |
| `src/simd.hpp` | AVX-512 / AVX2 / NEON / Scalar substring search, `-i` scan, NUL check, `\n` popcount, BP128 pack/unpack, SIMD intersection |
| `src/thread_pool.hpp` | Persistent lazily-started `WorkerPool` (zero per-query thread spawn, prevents glibc arena bloat) |
| `src/gitignore.hpp/.cpp` | Hierarchical `.gitignore` / `.git/info/exclude` / `.ignore`, glob filter, language table, depth-2 parallel directory walker |
| `src/io_uring_reader.hpp/.cpp` | Direct Linux `io_uring` syscall `OPENAT->READ->CLOSE` batching with `pread` fallback |
| `src/regex_engine.hpp/.cpp` | Recursive-descent Regex AST, OR-factored TrigramQuery planner, SIMD prefilter, Thompson NFA + thread-local lock-free Lazy DFA |
| `src/posting.hpp/.cpp` | Adaptive 3-tier compressed posting lists (`Inline`, block-skipping `BP128`, `Bitset`) |
| `src/index_format.hpp` | Zero-copy `mmap` v2 binary format (`IndexHeader`, `FileEntry`, `ChunkEntry`, `TrigramEntry`, `SymbolEntry`) |
| `src/symbols.hpp/.cpp` | Single-pass lexical definition extractor (C/C++, Rust, Python, Go, JS/TS, Java, Zig, Swift, Kotlin) |
| `src/indexer.hpp/.cpp` | Parallel chunked indexer, 64-way Fibonacci partitioning, stable LSD radix zero-sort merge, parallel symbol table |
| `src/delta.hpp/.cpp` | LSM `DeltaSegment`, lock-free `LiveIndex` snapshot swaps, compaction, daemon-less freshness patching |
| `src/searcher.hpp/.cpp` | Adaptive query planner, path pre-filtering, coalesced 32 KiB slice verification, SIMD linear fallback, stdin/multi-path, output formatters |
| `src/daemon.hpp/.cpp` | Linux `inotify` + macOS `FSEvents` coalescing watcher, `AF_UNIX` binary RPC server/client, persisted `repos.txt` |
| `src/tui.hpp/.cpp` | Raw `termios` split-pane interactive picker (`scry -I`) with live preview and atomic keystroke cancellation |
| `src/cli.hpp/.cpp`, `src/main.cpp` | `rg`-compatible CLI parser, auto-backgrounding daemon management, subcommands |
| `tests/test_main.cpp`, `tests/bench_engine.cpp` | Unit/integration test suite (ASan+UBSan clean) and C++ micro/macro benchmark tool (`scry-bench`) |
| `scripts/diff_test.sh`, `scripts/bench_matrix.py` | 23-case differential oracle vs `ripgrep` and reproducible 7-tool benchmark matrix |

---

## Build & Install

Requires a C++23 compiler (`g++ >= 13` or `clang++ >= 17`) and `make` (or `cmake`):

```bash
make -j$(nproc)          # Builds ./scry and ./scry-bench (-O3 -march=native -flto)
make install             # Installs scry to ~/.local/bin (override with PREFIX=/usr/local)
make test                # Builds and runs ./scry-test (10 unit & integration suites)
make DEBUG=1 test        # Runs full test suite under AddressSanitizer + UBSan
make diff-test           # Runs 23 byte-for-byte differential tests against ripgrep (rg)
make bench               # Runs scry-bench + CLI comparison vs rg
```

Optional: `make PORTABLE=1` builds without `-march=native`; `make strip` strips `./scry` to ~603 KB.

---

## Usage

### 1. Instant CLI (`rg`-compatible)

```bash
scry index /path/to/repo                           # Build zero-copy mmap index once
scry sched_entity /path/to/repo                    # Sub-2ms indexed literal search
scry -i schedentity /path/to/repo                  # Case-insensitive (hybrid lower table)
scry -S 'fn\s+[a-z0-9_]+_handler'                  # Smart-case regex
scry -e foo -e bar src tests                       # Multiple patterns & multiple paths
cat file.cpp | scry -n 'MmapIndex'                 # Stdin piping (auto-detected)
scry -F 'std::vector<DocId>' -g '*.hpp' -C 2       # Fixed string + glob + context
scry -t rust -c 'unwrap'                           # Language filter + counts (--type-list)
scry --vimgrep 'TrigramEntry'                      # Quickfix format (file:line:col:text)
scry --def MmapIndex                               # Sub-ms symbol definition lookup
scry --json --stats 'TrigramEntry'                 # ripgrep JSON Lines + planner telemetry
scry --files -g 'src/**'                           # Instant indexed file listing
```

### 2. Zero-Lag Interactive Picker

```bash
scry -I                      # Interactive split-pane picker with live preview
scry -I --open               # Press Enter to jump directly into $EDITOR / zed {file}:{line}:{col}
```

Keybindings: type to filter live, `Up`/`Down` or `Ctrl-P`/`Ctrl-N` to move, `Tab` to toggle regex/fixed, `Ctrl-R` to toggle case-insensitivity, `Ctrl-D` to toggle `--def`, `Enter` to select, `Esc`/`Ctrl-C` to quit.

### 3. Quiet Resident Indexer (Linux `inotify` + macOS `FSEvents`)

```bash
scry watch /path/to/repo          # Auto-starts background scryd, persists in ~/.config/scry/repos.txt
scry watch /path/to/other-repo    # Register an additional repo live
scry unwatch /path/to/other-repo  # Remove repo from live daemon & config
scry daemon status                # Show RSS, Private_Dirty, query count, and repo generations
scry daemon stop                  # Stop daemon cleanly
scry daemon start --foreground    # Foreground mode (for systemd / launchd)
```

See [`contrib/integrations.md`](contrib/integrations.md) for `systemd` (`contrib/scryd.service`), macOS `launchd` (`contrib/com.scry.daemon.plist`), Neovim Telescope, Vim `grepprg`, `tern` hotkey, `fzf`, and AI coding agent setups.

### Reproducing the 7-Tool Matrix

```bash
python3 scripts/bench_matrix.py /path/to/linux --work-dir /var/tmp --runs 7
./scripts/diff_test.sh /path/to/repo
```

## License

[MIT](LICENSE)
