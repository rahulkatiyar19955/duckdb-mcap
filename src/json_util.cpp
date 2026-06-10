#include "json_util.hpp"

#include <cctype>
#include <cstdint>

namespace duckdb {

namespace {

//! Parse exactly 4 hex digits at json[pos], advancing pos. False on malformed input.
bool ReadHex4(std::string_view json, size_t &pos, uint32_t &out) {
	if (pos + 4 > json.size()) {
		return false;
	}
	out = 0;
	for (int i = 0; i < 4; i++) {
		char h = json[pos + i];
		uint32_t v;
		if (h >= '0' && h <= '9') {
			v = static_cast<uint32_t>(h - '0');
		} else if (h >= 'a' && h <= 'f') {
			v = static_cast<uint32_t>(h - 'a' + 10);
		} else if (h >= 'A' && h <= 'F') {
			v = static_cast<uint32_t>(h - 'A' + 10);
		} else {
			return false;
		}
		out = (out << 4) | v;
	}
	pos += 4;
	return true;
}

void AppendUtf8(std::string &out, uint32_t cp) {
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

// Read a JSON string token starting at json[pos] == '"'. Returns the unescaped
// contents (standard escapes and unicode escapes, combining surrogate pairs;
// lone surrogates degrade to U+FFFD) and advances pos past the closing quote.
// If `out` is null, the token is skipped without materializing it.
bool ReadJsonString(std::string_view json, size_t &pos, std::string *out) {
	if (pos >= json.size() || json[pos] != '"') {
		return false;
	}
	pos++; // opening quote
	while (pos < json.size()) {
		char c = json[pos];
		if (c == '"') {
			pos++; // closing quote
			return true;
		}
		if (c == '\\' && pos + 1 < json.size()) {
			char esc = json[pos + 1];
			pos += 2;
			if (esc == 'u') {
				uint32_t cp;
				if (!ReadHex4(json, pos, cp)) {
					return false;
				}
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					// High surrogate: combine with a following low half if present.
					size_t save = pos;
					uint32_t low = 0;
					bool combined = false;
					if (pos + 1 < json.size() && json[pos] == '\\' && json[pos + 1] == 'u') {
						pos += 2;
						if (ReadHex4(json, pos, low) && low >= 0xDC00 && low <= 0xDFFF) {
							cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
							combined = true;
						}
					}
					if (!combined) {
						pos = save;
						cp = 0xFFFD;
					}
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					cp = 0xFFFD; // lone low surrogate
				}
				if (out) {
					AppendUtf8(*out, cp);
				}
			} else if (out) {
				switch (esc) {
				case 'n':
					out->push_back('\n');
					break;
				case 't':
					out->push_back('\t');
					break;
				case 'r':
					out->push_back('\r');
					break;
				case 'b':
					out->push_back('\b');
					break;
				case 'f':
					out->push_back('\f');
					break;
				default:
					out->push_back(esc); // quote, backslash, slash
					break;
				}
			}
			continue;
		}
		if (out) {
			out->push_back(c);
		}
		pos++;
	}
	return false; // unterminated
}

} // namespace

std::optional<std::string> ExtractJsonStringField(std::string_view json, std::string_view key) {
	size_t pos = 0;
	// Find the opening brace of the root object.
	while (pos < json.size() && json[pos] != '{') {
		pos++;
	}
	if (pos >= json.size()) {
		return std::nullopt;
	}
	pos++;         // past '{'
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
			if (pos < json.size() && (json[pos] == '{' || json[pos] == '[')) {
				// Container value: let the outer loop's depth tracking walk past it.
				// (A matching key with a container value is not a string field.)
				continue;
			}
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
			continue;
		}
		// Any other character (shouldn't normally happen at this position) — advance.
		pos++;
	}
	return std::nullopt;
}

} // namespace duckdb
