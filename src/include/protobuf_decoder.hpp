#pragma once

#include "schema_cache.hpp"

#include <mcap/types.hpp>

#include <memory>
#include <optional>
#include <string>

namespace duckdb {

//! Decodes protobuf-encoded MCAP messages to JSON using the FileDescriptorSet
//! embedded in each schema (no precompiled .proto files required). Stateful: it
//! caches the descriptor pool/factory across messages, so create one per scan and
//! reuse it. Not thread-safe (matches the single-threaded mcap_scan).
class ProtobufDecoder {
public:
	ProtobufDecoder();
	~ProtobufDecoder();

	ProtobufDecoder(const ProtobufDecoder &) = delete;
	ProtobufDecoder &operator=(const ProtobufDecoder &) = delete;

	//! Decode one protobuf message to a JSON string. Returns nullopt if the schema
	//! cannot be loaded or the payload cannot be parsed.
	std::optional<std::string> Decode(const McapChannelInfo &channel, const mcap::Message &message);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace duckdb
