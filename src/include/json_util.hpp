#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace duckdb {

//! Extract the value of a *top-level* object key as a string. This is a minimal
//! JSON scanner (not a full parser) that correctly skips string contents and
//! nested objects/arrays, so a key appearing inside a string value or a nested
//! object is never mistaken for the field we are looking for. String values are
//! unescaped (standard escapes and \uXXXX, including surrogate pairs);
//! non-string scalar values are returned as their raw token text.
std::optional<std::string> ExtractJsonStringField(std::string_view json, std::string_view key);

} // namespace duckdb
