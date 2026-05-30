#include "protobuf_decoder.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <unordered_set>

namespace duckdb {

namespace gp = google::protobuf;

struct ProtobufDecoder::Impl {
	gp::SimpleDescriptorDatabase db;
	gp::DescriptorPool pool;
	gp::DynamicMessageFactory factory;
	//! Schema ids whose FileDescriptorSet has already been added to the pool.
	std::unordered_set<mcap::SchemaId> loaded_schemas;

	Impl() : pool(&db) {
	}

	//! Add the schema's FileDescriptorSet to the pool (idempotent per schema id).
	bool LoadSchema(const McapChannelInfo &channel) {
		if (loaded_schemas.count(channel.schema_id)) {
			return true;
		}
		gp::FileDescriptorSet fd_set;
		if (!fd_set.ParseFromArray(channel.schema_data.data(), static_cast<int>(channel.schema_data.size()))) {
			return false;
		}
		gp::FileDescriptorProto unused;
		for (int i = 0; i < fd_set.file_size(); i++) {
			const auto &file = fd_set.file(i);
			// Adding the same file name twice errors; only add new files.
			if (!db.FindFileByName(file.name(), &unused)) {
				if (!db.Add(file)) {
					return false;
				}
			}
		}
		loaded_schemas.insert(channel.schema_id);
		return true;
	}
};

ProtobufDecoder::ProtobufDecoder() : impl(std::make_unique<Impl>()) {
}

ProtobufDecoder::~ProtobufDecoder() = default;

std::optional<std::string> ProtobufDecoder::Decode(const McapChannelInfo &channel, const mcap::Message &message) {
	if (channel.schema_data.empty() || channel.schema_name.empty()) {
		return std::nullopt;
	}

	const gp::Descriptor *descriptor = impl->pool.FindMessageTypeByName(channel.schema_name);
	if (descriptor == nullptr) {
		if (!impl->LoadSchema(channel)) {
			return std::nullopt;
		}
		descriptor = impl->pool.FindMessageTypeByName(channel.schema_name);
		if (descriptor == nullptr) {
			return std::nullopt;
		}
	}

	std::unique_ptr<gp::Message> proto(impl->factory.GetPrototype(descriptor)->New());
	if (!proto->ParseFromArray(message.data, static_cast<int>(message.dataSize))) {
		return std::nullopt;
	}

	gp::util::JsonPrintOptions options;
	options.preserve_proto_field_names = true;
	std::string json;
	auto status = gp::util::MessageToJsonString(*proto, &json, options);
	if (!status.ok()) {
		return std::nullopt;
	}
	return json;
}

} // namespace duckdb
