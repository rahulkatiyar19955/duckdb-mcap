#!/usr/bin/env python3
"""Generate a tiny protobuf-encoded MCAP fixture for sqllogictest.

Builds a FileDescriptorSet by hand (no .proto compilation needed), encodes a
couple of messages of a simple `test.Point` type, and writes them with
message_encoding="protobuf" / schema encoding="protobuf" — mirroring how real
foxglove/protobuf MCAPs embed their schema.

Run inside the project venv:

    . .venv/bin/activate
    pip install mcap protobuf
    python test/generate_protobuf_mcap.py
"""

from pathlib import Path

from google.protobuf import descriptor_pb2
from google.protobuf import descriptor_pool, message_factory
from mcap.writer import Writer

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "test" / "mcap" / "protobuf.mcap"


def build_descriptor_set() -> descriptor_pb2.FileDescriptorSet:
    """A single file 'test.proto' with message Point { double x; double y; string label; }."""
    file_proto = descriptor_pb2.FileDescriptorProto()
    file_proto.name = "test.proto"
    file_proto.package = "test"
    file_proto.syntax = "proto3"

    msg = file_proto.message_type.add()
    msg.name = "Point"
    LABEL_OPTIONAL = descriptor_pb2.FieldDescriptorProto.LABEL_OPTIONAL
    TYPE_DOUBLE = descriptor_pb2.FieldDescriptorProto.TYPE_DOUBLE
    TYPE_STRING = descriptor_pb2.FieldDescriptorProto.TYPE_STRING

    for num, (name, ftype) in enumerate(
        [("x", TYPE_DOUBLE), ("y", TYPE_DOUBLE), ("label", TYPE_STRING)], start=1
    ):
        f = msg.field.add()
        f.name = name
        f.number = num
        f.label = LABEL_OPTIONAL
        f.type = ftype

    fd_set = descriptor_pb2.FileDescriptorSet()
    fd_set.file.append(file_proto)
    return fd_set


def main() -> None:
    fd_set = build_descriptor_set()

    # Build a dynamic Point message class to serialize payloads.
    pool = descriptor_pool.DescriptorPool()
    for f in fd_set.file:
        pool.Add(f)
    point_desc = pool.FindMessageTypeByName("test.Point")
    Point = message_factory.GetMessageClass(point_desc)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as stream:
        writer = Writer(stream)
        writer.start(profile="")
        schema_id = writer.register_schema(
            name="test.Point",
            encoding="protobuf",
            data=fd_set.SerializeToString(),
        )
        chan = writer.register_channel(
            topic="/points",
            message_encoding="protobuf",
            schema_id=schema_id,
        )
        writer.add_message(
            channel_id=chan,
            log_time=1_700_000_000_000_000_000,
            publish_time=1_700_000_000_000_000_000,
            data=Point(x=1.5, y=2.5, label="alpha").SerializeToString(),
        )
        writer.add_message(
            channel_id=chan,
            log_time=1_700_000_001_000_000_000,
            publish_time=1_700_000_001_000_000_000,
            data=Point(x=-3.0, y=4.0, label="beta").SerializeToString(),
        )
        # All-default scalars (x=0, y=0): proto3 omits these on the wire, so this row
        # exercises that the decoder still emits zero-valued fields in the JSON.
        writer.add_message(
            channel_id=chan,
            log_time=1_700_000_002_000_000_000,
            publish_time=1_700_000_002_000_000_000,
            data=Point(x=0.0, y=0.0, label="origin").SerializeToString(),
        )
        writer.finish()


if __name__ == "__main__":
    main()
