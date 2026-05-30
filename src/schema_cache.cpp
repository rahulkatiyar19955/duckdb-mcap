#include "schema_cache.hpp"

namespace duckdb {

McapSchemaCache McapSchemaCache::FromReader(const mcap::McapReader &reader) {
	McapSchemaCache cache;
	auto schemas = reader.schemas();
	for (auto &entry : reader.channels()) {
		auto channel = entry.second;
		McapChannelInfo info;
		info.id = entry.first;
		info.topic = channel->topic;
		info.schema_id = channel->schemaId;
		info.message_encoding = channel->messageEncoding;

		auto schema_entry = schemas.find(channel->schemaId);
		if (schema_entry != schemas.end() && schema_entry->second) {
			info.schema_name = schema_entry->second->name;
			info.schema_encoding = schema_entry->second->encoding;
		}
		cache.channels.emplace(entry.first, std::move(info));
	}
	return cache;
}

const McapChannelInfo *McapSchemaCache::Lookup(mcap::ChannelId channel_id) const {
	auto entry = channels.find(channel_id);
	if (entry == channels.end()) {
		return nullptr;
	}
	return &entry->second;
}

} // namespace duckdb
