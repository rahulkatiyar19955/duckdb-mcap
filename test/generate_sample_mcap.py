#!/usr/bin/env python3
"""Generate a tiny JSON MCAP fixture for sqllogictest.

Run inside the project venv:

    python3 -m venv .venv
    . .venv/bin/activate
    pip install mcap
    python test/generate_sample_mcap.py
"""

from pathlib import Path

from mcap.writer import Writer


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "test" / "mcap" / "sample.mcap"


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as stream:
        writer = Writer(stream)
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


if __name__ == "__main__":
    main()
