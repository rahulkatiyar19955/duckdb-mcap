#include "ros2_decoder.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_set>

namespace duckdb {

//===--------------------------------------------------------------------===//
// .msg / ros2idl text parser
//===--------------------------------------------------------------------===//
namespace {

const std::unordered_set<std::string> &PrimitiveTypes() {
	static const std::unordered_set<std::string> prims = {
	    "bool",   "byte",   "char",   "int8",    "uint8",   "int16",  "uint16",
	    "int32",  "uint32", "int64",  "uint64",  "float32", "float64", "string",
	    "wstring"};
	return prims;
}

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

//! Normalize a type reference to "pkg/Type" (strip the ros2 "msg/" middle segment).
std::string NormalizeTypeName(const std::string &name) {
	// e.g. sensor_msgs/msg/Temperature -> sensor_msgs/Temperature
	auto first = name.find('/');
	if (first == std::string::npos) {
		return name;
	}
	auto second = name.find('/', first + 1);
	if (second == std::string::npos) {
		return name; // already pkg/Type
	}
	auto middle = name.substr(first + 1, second - first - 1);
	if (middle == "msg") {
		return name.substr(0, first) + "/" + name.substr(second + 1);
	}
	return name;
}

//! Parse a single field line (already comment-stripped, trimmed, non-empty).
//! Returns false for constants (TYPE NAME=value) and unparseable lines.
bool ParseFieldLine(const std::string &line, Ros2Field &out) {
	// Constants are `TYPE NAME = value` / `TYPE NAME=value`. A real field line never
	// contains '='. Detect it on the whole line (the '=' may be space-separated, e.g.
	// `uint8 INT8 = 1`), not just on the name token.
	if (line.find('=') != std::string::npos) {
		return false;
	}
	std::istringstream iss(line);
	std::string type_token, name_token;
	if (!(iss >> type_token >> name_token)) {
		return false;
	}

	// Array suffix on the type: T[], T[N], T[<=N].
	out.is_array = false;
	out.is_fixed_array = false;
	out.array_size = 0;
	auto bracket = type_token.find('[');
	std::string base_type = type_token;
	if (bracket != std::string::npos) {
		out.is_array = true;
		base_type = type_token.substr(0, bracket);
		auto close = type_token.find(']', bracket);
		std::string inner =
		    (close != std::string::npos) ? type_token.substr(bracket + 1, close - bracket - 1) : std::string();
		// Fixed array T[N] (not bounded "<=N").
		if (!inner.empty() && inner.find("<=") == std::string::npos) {
			try {
				out.array_size = static_cast<uint32_t>(std::stoul(inner));
				out.is_fixed_array = true;
			} catch (...) {
				out.is_fixed_array = false;
			}
		}
	}

	// Bounded string "string<=N" -> treat as string.
	auto le = base_type.find("<=");
	if (le != std::string::npos) {
		base_type = base_type.substr(0, le);
	}

	out.is_primitive = PrimitiveTypes().count(base_type) > 0;
	out.type = out.is_primitive ? base_type : NormalizeTypeName(base_type);
	out.name = name_token;
	return true;
}

//! Parse one block of lines into a message type (fields only).
Ros2MessageType ParseBlock(const std::string &name, const std::vector<std::string> &lines) {
	Ros2MessageType type;
	type.name = name;
	for (auto &raw : lines) {
		auto hash = raw.find('#');
		auto line = Trim(hash == std::string::npos ? raw : raw.substr(0, hash));
		if (line.empty()) {
			continue;
		}
		Ros2Field field;
		if (ParseFieldLine(line, field)) {
			type.fields.push_back(std::move(field));
		}
	}
	return type;
}

//! Package portion of a normalized "pkg/Type" name (empty if unqualified).
std::string PackageOf(const std::string &name) {
	auto slash = name.rfind('/');
	if (slash == std::string::npos) {
		return std::string();
	}
	return name.substr(0, slash);
}

//! Resolve a (possibly unqualified) field type ref against the set of known types
//! and the package of the enclosing type. ROS2 .msg allows referencing a type in
//! the same package by its bare name (e.g. `Transform` inside geometry_msgs), and
//! `Header` is shorthand for `std_msgs/Header`.
std::string ResolveTypeRef(const std::string &ref, const std::string &enclosing_package,
                           const std::unordered_map<std::string, Ros2MessageType> &types) {
	if (types.count(ref)) {
		return ref;
	}
	if (ref.find('/') == std::string::npos) {
		if (!enclosing_package.empty()) {
			auto qualified = enclosing_package + "/" + ref;
			if (types.count(qualified)) {
				return qualified;
			}
		}
		auto header = "std_msgs/" + ref;
		if (ref == "Header" && types.count(header)) {
			return header;
		}
	}
	return ref;
}

bool IsSeparatorLine(const std::string &line) {
	// A line of '=' (>= 3) delimits message definitions.
	auto t = Trim(line);
	if (t.size() < 3) {
		return false;
	}
	return t.find_first_not_of('=') == std::string::npos;
}

} // namespace

std::shared_ptr<Ros2Decoder::Schema> Ros2Decoder::LoadSchema(const McapChannelInfo &channel) {
	auto existing = schemas.find(channel.schema_id);
	if (existing != schemas.end()) {
		return existing->second;
	}

	auto schema = std::make_shared<Schema>();
	schema->root = NormalizeTypeName(channel.schema_name);

	std::string text(reinterpret_cast<const char *>(channel.schema_data.data()), channel.schema_data.size());

	// Split into blocks on separator lines; each non-root block starts with "MSG: name".
	std::vector<std::string> current;
	std::string current_name = schema->root;
	bool first_block = true;

	std::istringstream stream(text);
	std::string line;
	auto flush = [&]() {
		if (first_block) {
			schema->types[schema->root] = ParseBlock(schema->root, current);
			first_block = false;
		} else {
			auto norm = NormalizeTypeName(current_name);
			schema->types[norm] = ParseBlock(norm, current);
		}
		current.clear();
	};

	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (IsSeparatorLine(line)) {
			flush();
			current_name.clear();
			continue;
		}
		auto trimmed = Trim(line);
		if (current_name.empty() && trimmed.rfind("MSG:", 0) == 0) {
			current_name = Trim(trimmed.substr(4));
			continue;
		}
		current.push_back(line);
	}
	flush();

	// Second pass: resolve unqualified field type refs (e.g. `Transform` inside
	// geometry_msgs, or `Header`) now that every type name is known.
	for (auto &entry : schema->types) {
		auto package = PackageOf(entry.first);
		for (auto &field : entry.second.fields) {
			if (!field.is_primitive) {
				field.type = ResolveTypeRef(field.type, package, schema->types);
			}
		}
	}

	schemas.emplace(channel.schema_id, schema);
	return schema;
}

//===--------------------------------------------------------------------===//
// CDR reader (little/big endian, aligned) -> JSON
//===--------------------------------------------------------------------===//
namespace {

struct CdrReader {
	const uint8_t *data;
	size_t size;
	size_t pos = 0;
	bool little_endian = true;
	bool ok = true;

	bool Ensure(size_t n) {
		// Overflow-safe equivalent of `pos + n > size` (pos + n can wrap around).
		if (n > size || pos > size - n) {
			ok = false;
			return false;
		}
		return true;
	}

	void Align(size_t n) {
		// CDR aligns each primitive to its size, relative to the start of the body
		// (after the 4-byte encapsulation header).
		size_t rel = pos - 4;
		size_t pad = (n - (rel % n)) % n;
		pos += pad;
	}

	template <class T>
	T ReadRaw() {
		T v {};
		std::memcpy(&v, data + pos, sizeof(T));
		pos += sizeof(T);
		if (!little_endian && sizeof(T) > 1) {
			auto *bytes = reinterpret_cast<uint8_t *>(&v);
			std::reverse(bytes, bytes + sizeof(T));
		}
		return v;
	}

	template <class T>
	bool ReadPrimitive(T &out) {
		Align(sizeof(T));
		if (!Ensure(sizeof(T))) {
			return false;
		}
		out = ReadRaw<T>();
		return true;
	}

	uint32_t ReadLength() {
		uint32_t len = 0;
		ReadPrimitive(len);
		return len;
	}

	bool ReadString(std::string &out) {
		Align(4);
		if (!Ensure(4)) {
			return false;
		}
		uint32_t len = ReadRaw<uint32_t>();
		if (len == 0) {
			out.clear();
			return true;
		}
		if (!Ensure(len)) {
			return false;
		}
		// len includes the null terminator.
		size_t str_len = len > 0 ? len - 1 : 0;
		out.assign(reinterpret_cast<const char *>(data + pos), str_len);
		pos += len;
		return true;
	}
};

void JsonEscape(const std::string &in, std::string &out) {
	out.push_back('"');
	for (char c : in) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out.push_back(c);
			}
		}
	}
	out.push_back('"');
}

//! Append a primitive value of the given ros2 type, reading from the CDR stream.
bool AppendPrimitive(CdrReader &r, const std::string &type, std::string &out) {
	if (type == "bool") {
		uint8_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += v ? "true" : "false";
	} else if (type == "int8" || type == "char") {
		int8_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += std::to_string(static_cast<int>(v));
	} else if (type == "uint8" || type == "byte") {
		uint8_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += std::to_string(static_cast<unsigned>(v));
	} else if (type == "int16") {
		int16_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += std::to_string(v);
	} else if (type == "uint16") {
		uint16_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += std::to_string(v);
	} else if (type == "int32") {
		int32_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += std::to_string(v);
	} else if (type == "uint32") {
		uint32_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += std::to_string(v);
	} else if (type == "int64") {
		int64_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += std::to_string(v);
	} else if (type == "uint64") {
		uint64_t v;
		if (!r.ReadPrimitive(v)) return false;
		out += std::to_string(v);
	} else if (type == "float32") {
		float v;
		if (!r.ReadPrimitive(v)) return false;
		char buf[32];
		snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
		out += buf;
	} else if (type == "float64") {
		double v;
		if (!r.ReadPrimitive(v)) return false;
		char buf[32];
		snprintf(buf, sizeof(buf), "%.17g", v);
		out += buf;
	} else if (type == "string" || type == "wstring") {
		std::string s;
		if (!r.ReadString(s)) return false;
		JsonEscape(s, out);
	} else {
		return false;
	}
	return r.ok;
}

bool AppendValue(CdrReader &r, const Ros2Decoder::Schema &schema, const Ros2Field &field, std::string &out);

bool AppendMessage(CdrReader &r, const Ros2Decoder::Schema &schema, const std::string &type_name, std::string &out) {
	auto it = schema.types.find(type_name);
	if (it == schema.types.end()) {
		return false;
	}
	out.push_back('{');
	bool first = true;
	for (auto &field : it->second.fields) {
		if (!first) {
			out.push_back(',');
		}
		first = false;
		JsonEscape(field.name, out);
		out.push_back(':');
		if (!AppendValue(r, schema, field, out)) {
			return false;
		}
	}
	out.push_back('}');
	return r.ok;
}

bool AppendScalar(CdrReader &r, const Ros2Decoder::Schema &schema, const Ros2Field &field, std::string &out) {
	if (field.is_primitive) {
		return AppendPrimitive(r, field.type, out);
	}
	return AppendMessage(r, schema, field.type, out);
}

bool AppendValue(CdrReader &r, const Ros2Decoder::Schema &schema, const Ros2Field &field, std::string &out) {
	if (!field.is_array) {
		return AppendScalar(r, schema, field, out);
	}
	uint32_t count = field.is_fixed_array ? field.array_size : r.ReadLength();
	if (!r.ok) {
		return false;
	}
	out.push_back('[');
	for (uint32_t i = 0; i < count; i++) {
		if (i > 0) {
			out.push_back(',');
		}
		if (!AppendScalar(r, schema, field, out)) {
			return false;
		}
	}
	out.push_back(']');
	return r.ok;
}

} // namespace

//===--------------------------------------------------------------------===//
// Public entry
//===--------------------------------------------------------------------===//
Ros2Decoder::Ros2Decoder() = default;
Ros2Decoder::~Ros2Decoder() = default;

std::optional<std::string> Ros2Decoder::Decode(const McapChannelInfo &channel, const mcap::Message &message) {
	if (channel.schema_data.empty() || channel.schema_name.empty()) {
		return std::nullopt;
	}
	auto schema = LoadSchema(channel);
	if (!schema || schema->types.find(schema->root) == schema->types.end()) {
		return std::nullopt;
	}

	// CDR encapsulation header: [0x00, endianness, options(2)]. LE if byte 1 == 1.
	if (message.dataSize < 4) {
		return std::nullopt;
	}
	CdrReader reader;
	reader.data = reinterpret_cast<const uint8_t *>(message.data);
	reader.size = static_cast<size_t>(message.dataSize);
	reader.little_endian = (reader.data[1] == 1);
	reader.pos = 4; // skip encapsulation header

	std::string json;
	json.reserve(256);
	if (!AppendMessage(reader, *schema, schema->root, json) || !reader.ok) {
		return std::nullopt;
	}
	return json;
}

} // namespace duckdb
