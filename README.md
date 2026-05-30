# duckdb-mcap

A high-performance DuckDB extension that enables SQL-native querying of MCAP files.

## Current surface

- `mcap_scan(path)` streams MCAP messages with `timestamp`, `topic`, `schema`, `payload_blob`, `schema_name`, and JSON passthrough `payload_json`.
- `mcap_topics(path)`, `mcap_schemas(path)`, and `mcap_channels(path)` read summary metadata.
- `rosout(path)` exposes a small ROS-aware helper over JSON `/rosout` messages.
- Topic and timestamp filters are translated into MCAP `ReadMessageOptions`; projection pushdown skips JSON decoding when `payload_json` is not selected.

## Bootstrap

```bash
git submodule update --init --recursive
python3 -m venv .venv
. .venv/bin/activate
pip install mcap
python test/generate_sample_mcap.py
make
make test
```
