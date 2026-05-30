#include "decoder.hpp"

#include "protobuf_decoder.hpp"

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
                                             ProtobufDecoder *protobuf) {
	auto encoding = ToLower(channel.message_encoding);
	if (IsJsonEncoding(encoding)) {
		return std::string(reinterpret_cast<const char *>(message.data), static_cast<size_t>(message.dataSize));
	}
	if (encoding == "protobuf" && protobuf != nullptr) {
		return protobuf->Decode(channel, message);
	}
	return std::nullopt;
}

std::optional<std::string> ExtractJsonStringField(std::string_view json, std::string_view key) {
	const auto needle = "\"" + std::string(key) + "\"";
	auto pos = json.find(needle);
	if (pos == std::string_view::npos) {
		return std::nullopt;
	}
	pos = json.find(':', pos + needle.size());
	if (pos == std::string_view::npos) {
		return std::nullopt;
	}
	pos++;
	while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
		pos++;
	}
	if (pos >= json.size()) {
		return std::nullopt;
	}
	if (json[pos] != '"') {
		auto end = pos;
		while (end < json.size() && json[end] != ',' && json[end] != '}') {
			end++;
		}
		while (end > pos && std::isspace(static_cast<unsigned char>(json[end - 1]))) {
			end--;
		}
		return std::string(json.substr(pos, end - pos));
	}

	pos++;
	std::string result;
	for (; pos < json.size(); pos++) {
		auto c = json[pos];
		if (c == '\\' && pos + 1 < json.size()) {
			result.push_back(json[pos + 1]);
			pos++;
			continue;
		}
		if (c == '"') {
			return result;
		}
		result.push_back(c);
	}
	return std::nullopt;
}

} // namespace duckdb
