// Unit tests for the minimal JSON field extractor (src/json_util.cpp) used by
// the rosout helper view: top-level key lookup, correct unescaping (standard
// escapes, \uXXXX incl. surrogate pairs), nested-container skipping.
// Built and run by ./run_cpp_tests.sh (linked into the same Catch2 binary as
// test_pushdown.cpp, which owns CATCH_CONFIG_MAIN).

#include "catch.hpp"

#include "json_util.hpp"

using duckdb::ExtractJsonStringField;

TEST_CASE("extracts top-level string field", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"a":"x","b":"y"})", "b") == "y");
	REQUIRE(ExtractJsonStringField(R"(  { "a" : "x" })", "a") == "x");
}

TEST_CASE("unescapes standard escapes", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"m":"line1\nline2"})", "m") == "line1\nline2");
	REQUIRE(ExtractJsonStringField(R"({"m":"tab\there"})", "m") == "tab\there");
	REQUIRE(ExtractJsonStringField(R"({"m":"a \"quoted\" word"})", "m") == "a \"quoted\" word");
	REQUIRE(ExtractJsonStringField(R"({"m":"back\\slash"})", "m") == "back\\slash");
	REQUIRE(ExtractJsonStringField(R"({"m":"cr\rlf\fbs\b"})", "m") == "cr\rlf\fbs\b");
}

TEST_CASE("unescapes unicode escapes", "[json_util]") {
	// BMP code point: U+00E9 (e-acute) -> 2-byte UTF-8
	REQUIRE(ExtractJsonStringField(R"({"m":"A\u00e9"})", "m") == "A\xc3\xa9");
	// 3-byte UTF-8: U+20AC (euro sign)
	REQUIRE(ExtractJsonStringField(R"({"m":"\u20ac"})", "m") == "\xe2\x82\xac");
	// Surrogate pair: U+1F600 (emoji) -> 4-byte UTF-8
	REQUIRE(ExtractJsonStringField(R"({"m":"\ud83d\ude00"})", "m") == "\xf0\x9f\x98\x80");
	// Lone high surrogate degrades to U+FFFD instead of corrupting output
	REQUIRE(ExtractJsonStringField(R"({"m":"x\ud83dx"})", "m") == "x\xef\xbf\xbdx");
}

TEST_CASE("ignores nested keys and string-embedded braces", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"o":{"k":"inner"},"k":"outer"})", "k") == "outer");
	REQUIRE(ExtractJsonStringField(R"({"a":"fake } k","k":"real"})", "k") == "real");
	REQUIRE(ExtractJsonStringField(R"({"arr":["k","v"],"k":"real"})", "k") == "real");
}

TEST_CASE("returns non-string scalars as raw tokens", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"level":40})", "level") == "40");
	REQUIRE(ExtractJsonStringField(R"({"f": 1.5 , "x":"y"})", "f") == "1.5");
}

TEST_CASE("missing key and malformed input return nullopt", "[json_util]") {
	REQUIRE(!ExtractJsonStringField(R"({"a":"x"})", "b").has_value());
	REQUIRE(!ExtractJsonStringField(R"("not an object")", "a").has_value());
	REQUIRE(!ExtractJsonStringField(R"({"a":"unterminated)", "a").has_value());
	REQUIRE(!ExtractJsonStringField("", "a").has_value());
}
