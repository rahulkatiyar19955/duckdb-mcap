#include "decoder.hpp"

#include <algorithm>
#include <cctype>

namespace duckdb {

static bool IsJsonEncoding(std::string_view encoding) {
	auto lower = std::string(encoding);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	return lower == "json" || lower == "jsonschema" || lower == "application/json";
}

std::optional<std::string> DecodePayloadJson(std::string_view message_encoding, const mcap::Message &message) {
	if (!IsJsonEncoding(message_encoding)) {
		return std::nullopt;
	}
	return std::string(reinterpret_cast<const char *>(message.data), static_cast<size_t>(message.dataSize));
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
