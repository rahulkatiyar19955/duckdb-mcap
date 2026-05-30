# DuckDB-MCAP Extension: Implementation Guide

> **How to read this doc.** `plan.md` is the *what / why* (architecture, design
> principles, phase goals). This file is the *how*: concrete files, APIs, code
> skeletons, build commands, and tests for each phase. Each phase below maps
> back to the matching phase in `plan.md` and to the Success Criteria table
> (v0.1 → v1.0).

---

## 0. Prerequisites & Tooling

### 0.1 Install the DuckDB CLI (DuckDB skill)
The DuckDB skill (`duckdb/duckdb-skills`) is the primary dev companion for this
project. First make sure the CLI is present:

```bash
# Preferred: use the skill's installer
/duckdb-skills:install-duckdb

# Or manually
brew install duckdb            # macOS
# verify
duckdb --version
```

### 0.2 Toolchain
| Tool | Version | Purpose |
|------|---------|---------|
| C++ compiler | C++17 | Extension is C++ |
| CMake | ≥ 3.20 | Build system |
| make | any | Template wrapper targets |
| git | any | Submodules (DuckDB, extension-ci-tools) |
| vcpkg | latest | Dependency manager (MCAP, zstd, lz4) |

### 0.3 Dependencies (vendored via vcpkg)
- **MCAP C++ library** — header-only `mcap/reader.hpp` (source of truth reader).
- **zstd**, **lz4** — MCAP chunk compression codecs.
- DuckDB's bundled **`json`** extension — provides the `JSON` logical type (Phase 3+).

`vcpkg.json` manifest:
```json
{
  "dependencies": ["mcap", "zstd", "lz4"]
}
```

### 0.4 Authoritative reference sources (ALWAYS consult these first)
Do **not** rely on memory for APIs — look them up in these two sources before
writing or reviewing code in any phase:

| Topic | Source | How |
|-------|--------|-----|
| **DuckDB** — extensions, table functions, pushdown, types, SQL | **DuckDB skill** | Run `duckdb-docs "<query>"` (e.g. `duckdb-docs "filter pushdown table function"`). Use `query`/`read-file` to validate output. |
| **MCAP** — reader API, record types, field names, encodings | **`mcap_repo/`** (vendored) | Read the headers directly: `mcap_repo/cpp/mcap/include/mcap/reader.hpp` and `types.hpp` (this repo vendors MCAP C++ **v2.1.3**). These are the source of truth — prefer them over web docs. |

> Convention: each phase below lists the exact `duckdb-docs` lookups and the
> relevant `mcap_repo/` headers to read first.

### 0.5 Standing DuckDB-skill workflow (use throughout every phase)
| Skill command | When to use |
|---------------|-------------|
| `duckdb-docs <query>` | Look up extension / table-function / pushdown APIs **before** coding each phase. |
| `query <sql>` | Run ad-hoc SQL against the built extension or fixtures to validate output. |
| `read-file <path>` | Preview/profile a data file or MCAP fixture while debugging. |
| `read-memories` | Recall decisions/patterns from earlier sessions. |

### 0.6 Testing environment (use a virtual env when possible)
Isolate any Python-based tooling (test runners, MCAP fixture generation with the
`mcap` Python package, benchmark scripts) inside a virtual environment so the
system Python stays clean:

```bash
python3 -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
pip install mcap pytest            # fixture generation + test deps
# ... run tests ...
deactivate
```

- Add `.venv/` to `.gitignore`.
- The C++ extension tests run via `make test` (sqllogictest) and don't need the
  venv, but **MCAP fixture generation and any Python validation must run inside
  the activated venv**.

---

## Bootstrap — Extension scaffold
**Maps to:** `plan.md` Phase 1 architecture hooks · **Target:** extension loads.

Base the project on the official **`duckdb/extension-template`** (CMake + `make`,
`extension-ci-tools`, vcpkg toolchain).

> **Layout note:** `plan.md` sketches an `extension/` directory, but the official
> template uses `src/`. This guide standardizes on **`src/`** to stay compatible
> with the template's build wiring.

### Files
| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Extension build target, links MCAP/zstd/lz4 |
| `vcpkg.json` | Dependency manifest (§0.3) |
| `Makefile` | Template wrapper (`make`, `make test`, `make clean`) |
| `extension_config.cmake` | Registers the `mcap` extension with the DuckDB build |
| `src/mcap_extension.cpp` | Entry point: `mcap_init(...)`, `mcap_version()`, registration |
| `src/include/mcap_extension.hpp` | Public extension header |

### Entry point sketch
```cpp
// src/mcap_extension.cpp
static void LoadInternal(DatabaseInstance &db) {
    ExtensionUtil::RegisterFunction(db, GetMcapScanFunction());     // Phase 1/3
    ExtensionUtil::RegisterFunction(db, GetMcapTopicsFunction());   // Phase 2
    ExtensionUtil::RegisterFunction(db, GetMcapSchemasFunction());  // Phase 2
    ExtensionUtil::RegisterFunction(db, GetMcapChannelsFunction()); // Phase 2
}
extern "C" DUCKDB_EXTENSION_API void mcap_init(DatabaseInstance &db) { LoadInternal(db); }
extern "C" DUCKDB_EXTENSION_API const char *mcap_version() { return DuckDB::LibraryVersion(); }
```

### Verify
```bash
make
./build/release/duckdb -c "LOAD 'mcap'; SELECT 1;"
```

---

## Phase 1 — Skeleton scan (`mcap_scan`, raw BLOB)
**Maps to:** plan.md Phase 1 · **Target: v0.1** — `SELECT * FROM mcap_scan(...) LIMIT 100`.

**Goal:** stream messages from an MCAP file as rows with raw payload bytes (no decode).

### Files
- `src/mcap_scan.cpp`
- `src/include/mcap_scan.hpp`

### Output schema
| Column | Type |
|--------|------|
| `timestamp` | `TIMESTAMP` |
| `topic` | `VARCHAR` |
| `schema` | `VARCHAR` |
| `payload_blob` | `BLOB` |

### Implementation
- `duckdb-docs` first: `table function bind init scan`, `GlobalTableFunctionState`, `DataChunk FlatVector`.
- `Bind()` — open file with `mcap::McapReader::open(path)`; declare the fixed schema (`names` + `return_types`).
- `InitGlobalState()` — hold the reader + the `mcap::LinearMessageView` returned by
  `reader.readMessages()` (single-threaded for v0.1).
- `InitLocalState()` — minimal (per-thread cursor placeholder).
- `Scan()` — pull up to `STANDARD_VECTOR_SIZE` messages, fill the `DataChunk`
  (`timestamp` from `message.logTime`, `topic` from channel, `schema` from schema record,
  `payload_blob` from `message.data`/`dataSize`), set `output.SetCardinality(n)`.

### Test
`tests/sql/scan.test` (sqllogictest), fixture in `test/mcap/sample.mcap`:
```sql
LOAD 'mcap';
SELECT count(*) FROM mcap_scan('test/mcap/sample.mcap');
----
<expected>
```

---

## Phase 2 — Metadata functions (zero message scan)
**Maps to:** plan.md Phase 2.

**Goal:** instant metadata from the MCAP **summary section only** — never read payloads.

### Files
- `src/mcap_topics.cpp`, `src/mcap_schemas.cpp` (and a `mcap_channels` function, in `mcap_topics.cpp` or its own file).
- `src/schema_cache.cpp` — shared schema/channel registry reused by later phases.

### Functions & sources
| Function | Reads | Output columns |
|----------|-------|----------------|
| `mcap_topics(path)` | `reader.statistics()` + channels | `topic, type, count, start, end` |
| `mcap_schemas(path)` | `reader.schemas()` | `id, name, encoding, data` |
| `mcap_channels(path)` | `reader.channels()` | `id, topic, schema_id, message_encoding` |

### Implementation
- `duckdb-docs`: `read summary statistics channels schemas` (MCAP API), `table function projection`.
- Call `reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan)` once in `Bind`, then read
  the `statistics()` (`std::optional<Statistics>`), `channels()`, `schemas()` maps. Emit one row
  per entry — never call `readMessages()`.

### Test
`tests/sql/metadata.test` — assert counts/types match the fixture; `EXPLAIN` shows no payload scan.

---

## Phase 3 — Decoded JSON scan
**Maps to:** plan.md Phase 3.

**Goal:** return decoded payloads as `JSON`.

### Files
- `src/decoder.cpp`, `src/include/decoder.hpp`
- Extend `mcap_scan` schema: replace/add `payload_json JSON`, add `schema_name VARCHAR`.

### Output schema
| Column | Type |
|--------|------|
| `timestamp` | `TIMESTAMP` |
| `topic` | `VARCHAR` |
| `schema_name` | `VARCHAR` |
| `payload_json` | `JSON` |

### Implementation
- `duckdb-docs`: `json type logical type extension`, `JSONCommon`.
- Decode dispatch by `channel.messageEncoding`:
  - `json` / `jsonschema` → pass bytes straight through (v1 scope).
  - `ros1` / `cdr` / `protobuf` → decode to JSON. **Gate behind a follow-up** — v1 ships JSON passthrough first.
- **Decision (from plan.md):** emit JSON, **do not flatten** ROS messages yet (Phase 8).
- Depend on the bundled `json` extension for the `JSON` logical type.

### Test
`tests/sql/decode.test`:
```sql
SELECT payload_json->>'$.severity' FROM mcap_scan('test/mcap/sample.mcap') WHERE topic='/rosout' LIMIT 1;
----
ERROR
```

---

## Phase 4 — Predicate pushdown on `topic` (HIGHEST PRIORITY)
**Maps to:** plan.md Phase 4 · **Target: v0.2** — `WHERE topic = '/rosout'` pushdown.

**Goal:** DuckDB hands the filter to the extension; only the matching channel is read.

### Files
- `src/pushdown.cpp`, `src/include/pushdown.hpp`

### Implementation
- `duckdb-docs`: `filter pushdown TableFilterSet ConstantFilter pushdown_complex_filter`.
- On the `TableFunction`: set `filter_pushdown = true`.
- In `Bind`/`InitGlobal`, read the `TableFilterSet` for the `topic` column; translate
  `topic = '...'` and `topic IN (...)` into a set of MCAP **channel IDs**.
- Pass a `mcap::ReadMessageOptions` with a `topicFilter` (or matching channel-id set) to
  `reader.readMessages(onProblem, options)` so only matching channels are read.

### Test
`tests/sql/pushdown.test` — correctness for `WHERE topic='/rosout'`, plus assert reduced
work via `EXPLAIN ANALYZE` (rows scanned ≪ total).

---

## Phase 5 — Timestamp pushdown / chunk pruning
**Maps to:** plan.md Phase 5 · **Target: v0.3** — `WHERE timestamp BETWEEN ...` pruning.

**Goal:** read only chunks whose time range overlaps the predicate.

### Implementation (extends `src/pushdown.cpp`)
- `duckdb-docs`: `range filter pushdown timestamp`, MCAP `ChunkIndex`.
- Call `reader.readSummary(...)` then `reader.chunkIndexes()` in `Bind`/`InitGlobal`.
- For `WHERE timestamp BETWEEN a AND b`, keep only `ChunkIndex` entries where
  `messageStartTime <= b AND messageEndTime >= a`; also set `ReadMessageOptions::startTime`/
  `endTime` so the reader prunes at the source.
- Combine with Phase 4 channel filtering (prune on **both** topic and time).

### Test
`tests/sql/pruning.test` — time-range query returns correct rows and touches only N chunks.

---

## Phase 6 — Projection pushdown
**Maps to:** plan.md Phase 6.

**Goal:** materialize only requested columns; skip JSON decode when `payload_json` is not selected.

### Implementation
- `duckdb-docs`: `projection pushdown column_ids`.
- Set `projection_pushdown = true`; read `input.column_ids` in `Bind`.
- In `Scan`, only populate requested vectors; **skip the decoder entirely** when
  `payload_json` is not projected (the main saving).

### Test
`tests/sql/projection.test` — `SELECT timestamp, topic FROM mcap_scan(...)` produces no
decode work (assert via counter/EXPLAIN).

---

## Phase 7 — ROS-aware views
**Maps to:** plan.md Phase 7.

**Goal:** typed columns for common ROS topics instead of raw JSON.

### Files
- `src/views/rosout.cpp`, `src/views/diagnostics.cpp`, `src/views/odom.cpp`, `src/views/tf.cpp`
  — **or** a registered set of SQL macros over `mcap_scan`.

### Implementation
- Each view is a thin table function / macro that filters to the relevant topic(s) and
  projects known fields into typed columns, e.g. `rosout(path)` → `timestamp, severity, node, message`.
- `duckdb-docs`: `create macro table macro` (if going the macro route).

### Test
`tests/sql/rosout.test` — `SELECT severity, message FROM rosout('test/mcap/sample.mcap')`.

---

## Phase 8 — Nested field access *(deferred, post-v1.0)*
**Maps to:** plan.md Phase 8.

Future work only — schema flattening / `STRUCT` projection to support
`SELECT payload.pose.position.x` or flattened aliases (`position_x`). Requires full
schema flattening; **defer until after v1.0** per plan.md. No code this milestone.

---

## Phase 9 — DataPilot / MCP integration *(deferred)*
**Maps to:** plan.md Phase 9.

Expose a single `execute_sql(query)` tool so an agent drives everything through SQL
(`Question → SQL → Observe → SQL → Answer`). Note: the DuckDB **`query`** skill already
provides this loop for local validation today, so this phase is mostly packaging.

---

## Repository structure (reconciled to the template)
```
duckdb-mcap/
├── CMakeLists.txt
├── Makefile
├── vcpkg.json
├── extension_config.cmake
├── src/
│   ├── mcap_extension.cpp        # entry point & registration (Bootstrap)
│   ├── mcap_scan.cpp             # core scan (Phases 1, 3)
│   ├── mcap_topics.cpp           # metadata (Phase 2)
│   ├── mcap_schemas.cpp          # schema metadata (Phase 2)
│   ├── pushdown.cpp              # filter + projection pushdown (Phases 4, 5, 6)
│   ├── decoder.cpp               # message decoding (Phase 3)
│   ├── schema_cache.cpp          # schema & channel registry
│   ├── views/                    # ROS-aware views (Phase 7)
│   └── include/                  # headers
├── tests/sql/                    # sqllogictest: scan, metadata, decode,
│   │                             #   pushdown, pruning, projection, rosout
├── benchmark/                    # 1gb / 10gb / 100gb timing
└── test/mcap/                    # sample MCAP fixtures
```

---

## Success criteria (from plan.md)
| Version | Deliverable | Implemented by |
|---------|-------------|----------------|
| **v0.1** | `SELECT * FROM mcap_scan(...) LIMIT 100` | Bootstrap + Phase 1 |
| **v0.2** | `WHERE topic = '/rosout'` pushdown | Phase 4 |
| **v0.3** | `WHERE timestamp BETWEEN ...` chunk pruning | Phase 5 |
| **v1.0** | 10 GB MCAP, topic + time filter, **≪ 1 s** | Phases 4–6 combined |

**v1.0 target query:**
```sql
SELECT *
FROM mcap_scan('run.mcap')
WHERE topic = '/rosout'
  AND timestamp BETWEEN '2024-01-01' AND '2024-01-02';
```

---

## Build & test loop (every phase)
```bash
# 0. Activate the venv for any Python tooling (fixture gen, pytest, benchmarks)
source .venv/bin/activate

# 1. Build + run the C++ extension tests
make                                   # build extension
make test                              # run tests/sql/*.test (sqllogictest)

# 2. Ad-hoc validation with the DuckDB skill
query "LOAD 'mcap'; SELECT * FROM mcap_topics('test/mcap/sample.mcap');"
read-file test/mcap/sample.mcap        # inspect a fixture

# 3. Look things up — never guess:
duckdb-docs "filter pushdown table function"            # DuckDB API → DuckDB skill
#   MCAP API → read the vendored headers directly:
#   mcap_repo/cpp/mcap/include/mcap/reader.hpp, types.hpp
```
