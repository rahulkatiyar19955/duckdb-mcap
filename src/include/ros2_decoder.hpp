#pragma once

#include "schema_cache.hpp"

#include <mcap/types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {

//! One field of a ROS2 message definition.
struct Ros2Field {
	std::string type;     //! primitive name (e.g. "float64") or message ref (e.g. "std_msgs/Header")
	std::string name;     //! field name
	bool is_array = false;
	bool is_fixed_array = false;
	uint32_t array_size = 0; //! for fixed arrays
	bool is_primitive = false;
};

//! A parsed message type: an ordered list of fields.
struct Ros2MessageType {
	std::string name;
	std::vector<Ros2Field> fields;
};

//! Parses ros2msg/ros2idl schema text (root type + `MSG:`-delimited dependencies)
//! and decodes CDR-encoded payloads to JSON. Stateful: caches parsed schemas by id.
//! Create one per scan; not thread-safe.
class Ros2Decoder {
public:
	Ros2Decoder();
	~Ros2Decoder();

	Ros2Decoder(const Ros2Decoder &) = delete;
	Ros2Decoder &operator=(const Ros2Decoder &) = delete;

	std::optional<std::string> Decode(const McapChannelInfo &channel, const mcap::Message &message);

	//! A parsed schema: the normalized root type name + a table of all referenced types.
	struct Schema {
		std::string root;
		std::unordered_map<std::string, Ros2MessageType> types;
	};

private:
	//! schema id -> parsed type table (root + dependencies)
	std::unordered_map<mcap::SchemaId, std::shared_ptr<Schema>> schemas;

	std::shared_ptr<Schema> LoadSchema(const McapChannelInfo &channel);
};

} // namespace duckdb
