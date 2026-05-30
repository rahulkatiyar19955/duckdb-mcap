#!/usr/bin/env python3
"""Generate a tiny ROS2 (cdr / ros2msg) MCAP fixture for sqllogictest.

There is no pip ROS2 message writer here, so we hand-encode the CDR payload with
struct, honoring CDR alignment, for a small message type:

    test_msgs/Sample
      std_msgs/Header header      # builtin_interfaces/Time stamp + string frame_id
      float64[3] values           # fixed array
      string label

Run inside the project venv:

    . .venv/bin/activate
    pip install mcap
    python test/generate_ros2_mcap.py
"""

import struct
from pathlib import Path

from mcap.writer import Writer

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "test" / "mcap" / "ros2.mcap"

SCHEMA_TEXT = b"""\
std_msgs/Header header
float64[3] values
string label

================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id

================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
"""


class CdrWriter:
    """Little-endian CDR body writer with size-based alignment (relative to body start)."""

    def __init__(self):
        self.buf = bytearray()

    def _align(self, n):
        pad = (-len(self.buf)) % n
        self.buf.extend(b"\x00" * pad)

    def i32(self, v):
        self._align(4)
        self.buf.extend(struct.pack("<i", v))

    def u32(self, v):
        self._align(4)
        self.buf.extend(struct.pack("<I", v))

    def f64(self, v):
        self._align(8)
        self.buf.extend(struct.pack("<d", v))

    def string(self, s):
        data = s.encode("utf-8") + b"\x00"
        self.u32(len(data))
        self.buf.extend(data)


def encode_sample(sec, nanosec, frame_id, values, label):
    w = CdrWriter()
    # header.stamp.sec, header.stamp.nanosec
    w.i32(sec)
    w.u32(nanosec)
    # header.frame_id
    w.string(frame_id)
    # values[3] (fixed array, no length prefix)
    for v in values:
        w.f64(v)
    # label
    w.string(label)
    # 4-byte CDR encapsulation header: rep id (0x0001 = CDR LE) + options(2)
    return b"\x00\x01\x00\x00" + bytes(w.buf)


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as stream:
        writer = Writer(stream)
        writer.start(profile="ros2")
        schema_id = writer.register_schema(
            name="test_msgs/msg/Sample",
            encoding="ros2msg",
            data=SCHEMA_TEXT,
        )
        chan = writer.register_channel(
            topic="/sample",
            message_encoding="cdr",
            schema_id=schema_id,
        )
        writer.add_message(
            channel_id=chan,
            log_time=1_700_000_000_000_000_000,
            publish_time=1_700_000_000_000_000_000,
            data=encode_sample(100, 250, "base_link", [1.5, 2.5, 3.5], "alpha"),
        )
        writer.add_message(
            channel_id=chan,
            log_time=1_700_000_001_000_000_000,
            publish_time=1_700_000_001_000_000_000,
            data=encode_sample(101, 0, "odom", [-1.0, 0.0, 9.81], "beta"),
        )
        writer.finish()


if __name__ == "__main__":
    main()
