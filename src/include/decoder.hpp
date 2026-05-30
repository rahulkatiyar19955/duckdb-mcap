#pragma once

#include <mcap/types.hpp>

#include <optional>
#include <string>

namespace duckdb {

std::optional<std::string> DecodePayloadJson(std::string_view message_encoding, const mcap::Message &message);
std::optional<std::string> ExtractJsonStringField(std::string_view json, std::string_view key);

} // namespace duckdb
