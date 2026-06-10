#!/usr/bin/env python3
"""Generate tiny JSON MCAP fixtures for sqllogictest.

Writes three files:
  - sample.mcap      default (zstd-chunked) encoding + an attachment + metadata
  - sample_lz4.mcap  the same two messages with LZ4 chunk compression
  - empty.mcap       a valid file with no channels or messages

Run inside the project venv:

    python3 -m venv .venv
    . .venv/bin/activate
    pip install -r test/requirements.txt
    python test/generate_sample_mcap.py
"""

from pathlib import Path

from mcap.writer import CompressionType, Writer


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "test" / "mcap"


def write_messages(writer: Writer) -> None:
    writer.start(profile="json")
    schema_id = writer.register_schema(
        name="foxglove.Log",
        encoding="jsonschema",
        data=b'{"type":"object"}',
    )
    rosout = writer.register_channel(
        topic="/rosout",
        message_encoding="json",
        schema_id=schema_id,
    )
    odom = writer.register_channel(
        topic="/odom",
        message_encoding="json",
        schema_id=schema_id,
    )
    writer.add_attachment(
        create_time=1_700_000_000_000_000_000,
        log_time=1_700_000_000_000_000_000,
        name="notes.txt",
        media_type="text/plain",
        data=b"hello attachment",
    )
    writer.add_metadata("session", {"vehicle": "amr-7", "site": "plant-3"})
    writer.add_message(
        channel_id=rosout,
        log_time=1_700_000_000_000_000_000,
        publish_time=1_700_000_000_000_000_000,
        data=b'{"severity":"ERROR","node":"planner","message":"Planner aborted"}',
    )
    writer.add_message(
        channel_id=odom,
        log_time=1_700_000_100_000_000_000,
        publish_time=1_700_000_100_000_000_000,
        data=b'{"x":1.0}',
    )
    writer.finish()


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    with (OUT_DIR / "sample.mcap").open("wb") as stream:
        write_messages(Writer(stream))
    # LZ4 chunk compression: the lz4 code path has the most fragile build/link
    # story, so keep it exercised by the test suite.
    with (OUT_DIR / "sample_lz4.mcap").open("wb") as stream:
        write_messages(Writer(stream, compression=CompressionType.LZ4))
    # A valid, completely empty file: scans must return zero rows, not errors.
    with (OUT_DIR / "empty.mcap").open("wb") as stream:
        writer = Writer(stream)
        writer.start(profile="json")
        writer.finish()


if __name__ == "__main__":
    main()
