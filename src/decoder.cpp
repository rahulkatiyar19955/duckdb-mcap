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

namespace {

// Read a JSON string token starting at json[pos] == '"'. Returns the unescaped
// contents and advances pos past the closing quote. If `out` is null, the token
// is skipped without materializing it.
bool ReadJsonString(std::string_view json, size_t &pos, std::string *out) {
	if (pos >= json.size() || json[pos] != '"') {
		return false;
	}
	pos++; // opening quote
	for (; pos < json.size(); pos++) {
		char c = json[pos];
		if (c == '\\' && pos + 1 < json.size()) {
			if (out) {
				out->push_back(json[pos + 1]);
			}
			pos++;
			continue;
		}
		if (c == '"') {
			pos++; // closing quote
			return true;
		}
		if (out) {
			out->push_back(c);
		}
	}
	return false; // unterminated
}

} // namespace

// Extract the value of a *top-level* object key as a string. This is a minimal
// JSON scanner (not a full parser) that correctly skips string contents and
// nested objects/arrays, so a key appearing inside a string value or a nested
// object is never mistaken for the field we are looking for.
std::optional<std::string> ExtractJsonStringField(std::string_view json, std::string_view key) {
	size_t pos = 0;
	// Find the opening brace of the root object.
	while (pos < json.size() && json[pos] != '{') {
		pos++;
	}
	if (pos >= json.size()) {
		return std::nullopt;
	}
	pos++; // past '{'
	int depth = 0; // nesting depth relative to the root object body

	while (pos < json.size()) {
		char c = json[pos];
		if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
			pos++;
			continue;
		}
		if (c == '}') {
			if (depth == 0) {
				return std::nullopt; // end of root object
			}
			depth--;
			pos++;
			continue;
		}
		if (c == '{' || c == '[') {
			depth++;
			pos++;
			continue;
		}
		if (c == ']') {
			depth--;
			pos++;
			continue;
		}
		if (c == '"') {
			// A string token. Only at depth 0 can it be an object key.
			std::string token;
			size_t key_start = pos;
			if (!ReadJsonString(json, pos, &token)) {
				return std::nullopt;
			}
			if (depth != 0) {
				continue; // string value/element inside a nested container
			}
			// Determine whether this string is a key (followed by ':').
			size_t after = pos;
			while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) {
				after++;
			}
			if (after >= json.size() || json[after] != ':') {
				continue; // a bare string value at depth 0 (unusual) — skip
			}
			pos = after + 1; // past ':'
			while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
				pos++;
			}
			bool is_match = (token == key);
			if (pos < json.size() && json[pos] == '"') {
				std::string value;
				if (!ReadJsonString(json, pos, is_match ? &value : nullptr)) {
					return std::nullopt;
				}
				if (is_match) {
					return value;
				}
			} else {
				// Non-string scalar value: read until separator/closer at depth 0.
				size_t start = pos;
				while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']') {
					pos++;
				}
				if (is_match) {
					size_t end = pos;
					while (end > start && std::isspace(static_cast<unsigned char>(json[end - 1]))) {
						end--;
					}
					return std::string(json.substr(start, end - start));
				}
			}
			(void)key_start;
			continue;
		}
		// Any other character (shouldn't normally happen at this position) — advance.
		pos++;
	}
	return std::nullopt;
}

} // namespace duckdb
