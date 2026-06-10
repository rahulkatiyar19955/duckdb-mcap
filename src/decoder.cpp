#include "decoder.hpp"

#include "protobuf_decoder.hpp"
#include "ros2_decoder.hpp"

#include <algorithm>
#include <cctype>

namespace duckdb {

static std::string ToLower(std::string_view encoding) {
	std::string lower(encoding);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	return lower;
}

static bool IsJsonEncoding(const std::string &lower) {
	return lower == "json" || lower == "jsonschema" || lower == "application/json";
}

std::optional<std::string> DecodePayloadJson(const McapChannelInfo &channel, const mcap::Message &message,
                                             ProtobufDecoder *protobuf, Ros2Decoder *ros2) {
	auto encoding = ToLower(channel.message_encoding);
	if (IsJsonEncoding(encoding)) {
		return std::string(reinterpret_cast<const char *>(message.data), static_cast<size_t>(message.dataSize));
	}
	if (encoding == "protobuf" && protobuf != nullptr) {
		return protobuf->Decode(channel, message);
	}
	if (encoding == "cdr" && ros2 != nullptr) {
		return ros2->Decode(channel, message);
	}
	return std::nullopt;
}

} // namespace duckdb
