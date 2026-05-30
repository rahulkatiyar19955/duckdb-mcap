#pragma once

#include "schema_cache.hpp"

#include <mcap/types.hpp>

#include <optional>
#include <string>

namespace duckdb {

class ProtobufDecoder;

//! Decode a message payload to JSON based on its channel/schema encoding.
//! Supports: json/jsonschema (passthrough) and protobuf (when `protobuf` is
//! provided). Returns nullopt for unsupported encodings or decode failures.
std::optional<std::string> DecodePayloadJson(const McapChannelInfo &channel, const mcap::Message &message,
                                             ProtobufDecoder *protobuf);

//! Minimal JSON string-field extractor used by the rosout helper view.
std::optional<std::string> ExtractJsonStringField(std::string_view json, std::string_view key);

} // namespace duckdb
