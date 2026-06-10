#include "protobuf_decoder.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

namespace gp = google::protobuf;

struct ProtobufDecoder::Impl {
	gp::SimpleDescriptorDatabase db;
	gp::DescriptorPool pool;
	gp::DynamicMessageFactory factory;
	//! Schema ids whose FileDescriptorSet has already been added to the pool.
	std::unordered_set<mcap::SchemaId> loaded_schemas;
	//! One reusable dynamic message per schema id, so per-row decoding is a
	//! Clear()+ParseFromArray instead of a descriptor lookup plus an allocation.
	//! nullptr entries mark schemas that failed to resolve (avoid re-trying per row).
	std::unordered_map<mcap::SchemaId, std::unique_ptr<gp::Message>> instances;

	Impl() : pool(&db) {
	}

	//! Resolve (once per schema id) the reusable message instance for a channel.
	gp::Message *GetInstance(const McapChannelInfo &channel) {
		auto cached = instances.find(channel.schema_id);
		if (cached != instances.end()) {
			return cached->second.get();
		}
		std::unique_ptr<gp::Message> instance;
		const gp::Descriptor *descriptor = pool.FindMessageTypeByName(channel.schema_name);
		if (descriptor == nullptr && LoadSchema(channel)) {
			descriptor = pool.FindMessageTypeByName(channel.schema_name);
		}
		if (descriptor != nullptr) {
			instance.reset(factory.GetPrototype(descriptor)->New());
		}
		auto *result = instance.get();
		instances.emplace(channel.schema_id, std::move(instance));
		return result;
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

	auto *proto = impl->GetInstance(channel);
	if (proto == nullptr) {
		return std::nullopt;
	}
	proto->Clear();
	if (!proto->ParseFromArray(message.data, static_cast<int>(message.dataSize))) {
		return std::nullopt;
	}

	gp::util::JsonPrintOptions options;
	options.preserve_proto_field_names = true;
	// Emit zero/default-valued proto3 scalars too (they are omitted by default), so a
	// Point with x=0 still decodes to {"x":0,...} and SQL field extraction is stable.
	// The option was renamed in protobuf v26 (5026000); keep the old name for the
	// 3.x/4.x toolchains still used on some CI distros.
#if defined(GOOGLE_PROTOBUF_VERSION) && GOOGLE_PROTOBUF_VERSION >= 5026000
	options.always_print_fields_with_no_presence = true;
#else
	options.always_print_primitive_fields = true;
#endif
	std::string json;
	auto status = gp::util::MessageToJsonString(*proto, &json, options);
	if (!status.ok()) {
		return std::nullopt;
	}
	return json;
}

} // namespace duckdb
