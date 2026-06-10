# duckdb-mcap

A high-performance DuckDB extension that enables SQL-native querying of
[MCAP](https://mcap.dev) files.

Built against **DuckDB v1.4.4** (pinned via submodules).

## Current surface

- `mcap_scan(path)` streams MCAP messages. `path` may be a single file, a glob
  pattern (`runs/*.mcap`), or any path DuckDB's filesystems understand
  (including `httpfs`/S3 once that extension is loaded). Columns:

  | column | type | |
  |--------|------|---|
  | `timestamp` | `TIMESTAMP_NS` | message log time, full nanosecond precision |
  | `topic` | `VARCHAR` | channel topic |
  | `payload_blob` | `BLOB` | raw message bytes |
  | `schema_name` | `VARCHAR` | schema name of the channel |
  | `payload_json` | `JSON` | decoded payload (see below), NULL if undecodable |
  | `publish_time` | `TIMESTAMP_NS` | message publish time |
  | `sequence` | `UINTEGER` | per-channel sequence number |
  | `channel_id` | `USMALLINT` | MCAP channel id |
  | `filename` | `VARCHAR` | source file (useful with globs) |

- `FROM 'run.mcap'` works directly (replacement scan).
- Scans run **in parallel**: each file's chunk index is split into time-range
  partitions consumed by DuckDB's worker threads.
- `mcap_topics(path)` — per-topic type, message count, and **exact per-topic**
  `start`/`end` time ranges (from message indexes; NULL if the file has none).
- `mcap_channels(path)`, `mcap_schemas(path)` — channel/schema registry.
- `mcap_info(path)` — header + statistics (profile, library, counts, time range).
- `mcap_attachments(path)`, `mcap_metadata(path)` — attachment and metadata
  records.
- `rosout(path)` — ROS-aware view over `/rosout` logs. Handles both JSON and
  CDR (`rcl_interfaces/msg/Log`) encodings; numeric levels map to
  DEBUG/INFO/WARN/ERROR/FATAL.
- **Pushdown**: topic and timestamp filters are translated into MCAP
  `ReadMessageOptions` (channel/chunk pruning); projection pushdown skips JSON
  decoding when `payload_json` is not selected. Cardinality estimates come from
  MCAP statistics.

## Payload decoding (`payload_json`)

| Encoding | Support | How |
|----------|---------|-----|
| `json` / `jsonschema` | ✅ | Passthrough |
| `protobuf` | ✅ | Dynamic decode from the embedded `FileDescriptorSet` (no precompiled `.proto`) via libprotobuf, emitted as JSON |
| `cdr` (ROS2) | ✅ | `ros2msg`/`ros2idl` schema parser + CDR reader → JSON (nested types, arrays, strings; LE and BE) |
| `cdr` (XCDR2 / PL_CDR) | ❌ | Rejected (NULL) rather than misparsed |
| `ros1` | ❌ | Out of scope (deprecated) |

Unsupported encodings (and `wstring` fields, whose CDR layout is
vendor-ambiguous) yield `NULL` for `payload_json` instead of garbage.

## Example

```sql
LOAD 'mcap';
LOAD 'json';

-- metadata
SELECT * FROM mcap_info('run.mcap');
SELECT * FROM mcap_topics('run.mcap');

-- decoded protobuf / ros2 fields, across a directory of recordings
SELECT timestamp, filename, payload_json->>'$.header.frame_id' AS frame
FROM mcap_scan('runs/*.mcap')
WHERE topic = '/vectornav/IMU_restamped'
  AND timestamp BETWEEN TIMESTAMP '2024-01-01' AND TIMESTAMP '2024-01-02';

-- or just
SELECT count(*) FROM 'run.mcap';
```

## Build dependencies

- DuckDB + extension-ci-tools submodules (pinned to v1.4.4)
- `lz4`, `zstd` (MCAP chunk compression) — e.g. `brew install lz4 zstd`
- `protobuf` (protobuf payload decoding) — e.g. `brew install protobuf`

## Bootstrap

```bash
git submodule update --init --recursive
python3 -m venv .venv
. .venv/bin/activate
pip install -r test/requirements.txt
python test/generate_sample_mcap.py
python test/generate_protobuf_mcap.py
python test/generate_ros2_mcap.py
make
make test
```

## Fast dev loop

```bash
./run_tests.sh              # incremental build + run all test/sql/*.test, then the C++ unit tests
./run_tests.sh --no-build   # just run tests
./run_tests.sh protobuf     # build + run a single test
```

## Tests

- **SQL** (`test/sql/*.test`): end-to-end sqllogictests over generated MCAP
  fixtures, run via DuckDB's `unittest` binary (`make test`). Fixture
  generators are pinned via `test/requirements.txt`; CI regenerates the
  fixtures and fails on drift.
- **C++** (`test/cpp/*.cpp`): Catch2 unit tests for the pushdown filter logic
  (AND/OR/IN, range hull, overflow saturation) and the JSON field extractor
  (escapes, unicode, nesting). Build + run with `./run_cpp_tests.sh` (links the
  `libduckdb` from a prior build); also run as part of a full `./run_tests.sh`.
