#pragma once

#include <mcap/reader.hpp>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {

struct McapChannelInfo {
	mcap::ChannelId id;
	std::string topic;
	mcap::SchemaId schema_id;
	std::string message_encoding;
	std::string schema_name;
	std::string schema_encoding;
	//! Raw schema definition bytes (e.g. protobuf FileDescriptorSet, ros2msg text).
	std::vector<std::byte> schema_data;
};

struct McapSchemaCache {
	std::unordered_map<mcap::ChannelId, McapChannelInfo> channels;

	static McapSchemaCache FromReader(const mcap::McapReader &reader);
	const McapChannelInfo *Lookup(mcap::ChannelId channel_id) const;
};

} // namespace duckdb
