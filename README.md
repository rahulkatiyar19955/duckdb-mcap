# duckdb-mcap

A high-performance DuckDB extension that enables SQL-native querying of MCAP files.

Built against **DuckDB v1.4.4** (pinned via submodules).

## Current surface

- `mcap_scan(path)` streams MCAP messages with `timestamp`, `topic`,
  `payload_blob`, `schema_name`, and decoded `payload_json`.
- `mcap_topics(path)`, `mcap_schemas(path)`, and `mcap_channels(path)` read
  summary metadata instantly (no message scan).
- `rosout(path)` exposes a small ROS-aware helper over JSON `/rosout` messages.
- **Pushdown**: topic and timestamp filters are translated into MCAP
  `ReadMessageOptions` (channel/chunk pruning); projection pushdown skips JSON
  decoding when `payload_json` is not selected.

## Payload decoding (`payload_json`)

| Encoding | Support | How |
|----------|---------|-----|
| `json` / `jsonschema` | ✅ | Passthrough |
| `protobuf` | ✅ | Dynamic decode from the embedded `FileDescriptorSet` (no precompiled `.proto`) via libprotobuf, emitted as JSON |
| `cdr` (ROS2) | ✅ | `ros2msg`/`ros2idl` schema parser + CDR reader → JSON (nested types, arrays, strings) |
| `ros1` | ❌ | Out of scope (deprecated) |

Unsupported encodings yield `NULL` for `payload_json`.

## Example

```sql
LOAD 'mcap';
LOAD 'json';

-- metadata
SELECT * FROM mcap_topics('run.mcap');

-- decoded protobuf / ros2 fields
SELECT timestamp, topic, payload_json->>'$.header.frame_id' AS frame
FROM mcap_scan('run.mcap')
WHERE topic = '/vectornav/IMU_restamped'
  AND timestamp BETWEEN TIMESTAMP '2024-01-01' AND TIMESTAMP '2024-01-02';
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
pip install mcap protobuf
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
  fixtures, run via DuckDB's `unittest` binary (`make test`).
- **C++** (`test/cpp/*.cpp`): Catch2 unit tests for the pushdown filter logic
  (AND/OR/IN, range hull, overflow saturation). Build + run with
  `./run_cpp_tests.sh` (links the `libduckdb` from a prior build); also run as
  part of a full `./run_tests.sh`.
