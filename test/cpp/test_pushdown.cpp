// Standalone C++ unit tests for the MCAP pushdown logic.
//
// These exercise CollectTopicSet / CollectTimeRange (src/pushdown.cpp) directly
// against synthetic DuckDB TableFilter trees — the AND/OR/IN/hull/overflow edge
// cases that the SQL sqllogictests can only reach indirectly. Built and run by
// ./run_cpp_tests.sh (links the already-built libduckdb).

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "pushdown.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"

#include <string>
#include <unordered_set>
#include <vector>

using namespace duckdb;

namespace {

using TopicSet = std::unordered_set<std::string>;

//! micros -> nanos factor: helpers below take microseconds for readability and
//! build TIMESTAMP_NS constants (the scan's timestamp column type) in nanoseconds.
constexpr mcap::Timestamp NS = 1000;

unique_ptr<TableFilter> TopicEqual(const std::string &topic) {
	return make_uniq<ConstantFilter>(ExpressionType::COMPARE_EQUAL, Value(topic));
}

unique_ptr<TableFilter> TopicIn(const std::vector<std::string> &topics) {
	vector<Value> values;
	for (auto &topic : topics) {
		values.emplace_back(Value(topic));
	}
	return make_uniq<InFilter>(std::move(values));
}

//! A topic predicate that is NOT a finite set (e.g. `topic > 'x'`).
unique_ptr<TableFilter> TopicGreaterThan(const std::string &topic) {
	return make_uniq<ConstantFilter>(ExpressionType::COMPARE_GREATERTHAN, Value(topic));
}

unique_ptr<TableFilter> TsCompare(ExpressionType cmp, int64_t micros) {
	return make_uniq<ConstantFilter>(cmp, Value::TIMESTAMPNS(timestamp_ns_t(micros * 1000)));
}

unique_ptr<ConjunctionAndFilter> And(unique_ptr<TableFilter> a, unique_ptr<TableFilter> b) {
	auto conjunction = make_uniq<ConjunctionAndFilter>();
	conjunction->child_filters.push_back(std::move(a));
	conjunction->child_filters.push_back(std::move(b));
	return conjunction;
}

unique_ptr<ConjunctionOrFilter> Or(unique_ptr<TableFilter> a, unique_ptr<TableFilter> b) {
	auto disjunction = make_uniq<ConjunctionOrFilter>();
	disjunction->child_filters.push_back(std::move(a));
	disjunction->child_filters.push_back(std::move(b));
	return disjunction;
}

} // namespace

//===--------------------------------------------------------------------===//
// Topic predicate -> finite set
//===--------------------------------------------------------------------===//

TEST_CASE("topic equality is a single-element set", "[pushdown][topic]") {
	TopicSet out;
	REQUIRE(CollectTopicSet(out, *TopicEqual("/odom")));
	REQUIRE(out == TopicSet {"/odom"});
}

TEST_CASE("topic IN collects every value", "[pushdown][topic]") {
	TopicSet out;
	REQUIRE(CollectTopicSet(out, *TopicIn({"/a", "/b", "/c"})));
	REQUIRE(out == TopicSet {"/a", "/b", "/c"});
}

TEST_CASE("topic OR unions its branches", "[pushdown][topic]") {
	TopicSet out;
	REQUIRE(CollectTopicSet(out, *Or(TopicEqual("/a"), TopicEqual("/b"))));
	REQUIRE(out == TopicSet {"/a", "/b"});
}

TEST_CASE("topic OR with an unrepresentable branch does not restrict (no pollution)", "[pushdown][topic]") {
	// `/a OR topic > /b`: the second branch is not a finite set, so the disjunction
	// cannot be bounded and must leave the scan unconstrained — without leaving "/a"
	// behind in the output set.
	TopicSet out;
	REQUIRE_FALSE(CollectTopicSet(out, *Or(TopicEqual("/a"), TopicGreaterThan("/b"))));
	REQUIRE(out.empty());
}

TEST_CASE("topic AND keeps the representable child without leaking the other", "[pushdown][topic]") {
	// `/a AND topic > /b` reduces to (a superset of) {"/a"}; the unrepresentable
	// child must not leak "/b" into the set.
	TopicSet out;
	REQUIRE(CollectTopicSet(out, *And(TopicEqual("/a"), TopicGreaterThan("/b"))));
	REQUIRE(out == TopicSet {"/a"});
}

TEST_CASE("nested AND/OR does not leak from a failed OR branch", "[pushdown][topic]") {
	// `(/a OR topic > /x) AND /c`: the OR is unrepresentable and contributes nothing;
	// only "/c" should survive.
	TopicSet out;
	auto filter = And(Or(TopicEqual("/a"), TopicGreaterThan("/x")), TopicEqual("/c"));
	REQUIRE(CollectTopicSet(out, *filter));
	REQUIRE(out == TopicSet {"/c"});
}

//===--------------------------------------------------------------------===//
// Timestamp predicate -> contiguous range
//===--------------------------------------------------------------------===//

TEST_CASE("timestamp comparisons map to half-open ranges", "[pushdown][timestamp]") {
	mcap::Timestamp start = 0;
	mcap::Timestamp end = 0;

	REQUIRE(CollectTimeRange(start, end, *TsCompare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, 1000)));
	REQUIRE(start == 1000 * NS);
	REQUIRE(end == mcap::MaxTime);

	REQUIRE(CollectTimeRange(start, end, *TsCompare(ExpressionType::COMPARE_LESSTHAN, 2000)));
	REQUIRE(start == 0);
	REQUIRE(end == 2000 * NS);
}

TEST_CASE("timestamp AND intersects child ranges", "[pushdown][timestamp]") {
	auto filter = And(TsCompare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, 1000),
	                  TsCompare(ExpressionType::COMPARE_LESSTHAN, 5000));
	mcap::Timestamp start = 0;
	mcap::Timestamp end = 0;
	REQUIRE(CollectTimeRange(start, end, *filter));
	REQUIRE(start == 1000 * NS);
	REQUIRE(end == 5000 * NS);
}

TEST_CASE("timestamp OR widens to the hull instead of inverting (regression)", "[pushdown][timestamp]") {
	// `ts < 1000 OR ts > 5000` is two disjoint ranges. The old code intersected them
	// into an empty/inverted window and silently dropped every row; the hull must stay
	// a valid (non-inverted) range.
	auto filter = Or(TsCompare(ExpressionType::COMPARE_LESSTHAN, 1000),
	                 TsCompare(ExpressionType::COMPARE_GREATERTHAN, 5000));
	mcap::Timestamp start = 42;
	mcap::Timestamp end = 42;
	REQUIRE(CollectTimeRange(start, end, *filter));
	REQUIRE(start <= end);         // never inverted
	REQUIRE(start == 0);           // hull lower bound
	REQUIRE(end == mcap::MaxTime); // hull upper bound
}

TEST_CASE("timestamp OR with an unrepresentable branch is not bounded", "[pushdown][timestamp]") {
	// COMPARE_NOTEQUAL is not a contiguous range, so the disjunction cannot be bounded.
	auto filter = Or(TsCompare(ExpressionType::COMPARE_LESSTHAN, 1000),
	                 TsCompare(ExpressionType::COMPARE_NOTEQUAL, 5000));
	mcap::Timestamp start = 0;
	mcap::Timestamp end = 0;
	REQUIRE_FALSE(CollectTimeRange(start, end, *filter));
}

TEST_CASE("non-NS timestamp constants convert via cast", "[pushdown][timestamp]") {
	// A plain TIMESTAMP (microsecond) constant is cast to nanoseconds.
	auto filter =
	    make_uniq<ConstantFilter>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::TIMESTAMP(timestamp_t(1500)));
	mcap::Timestamp start = 0;
	mcap::Timestamp end = 0;
	REQUIRE(CollectTimeRange(start, end, *filter));
	REQUIRE(start == 1500 * NS);
	REQUIRE(end == mcap::MaxTime);
}

TEST_CASE("unconvertible timestamp constants leave the window unrestricted", "[pushdown][timestamp]") {
	// A microsecond timestamp too large for int64 nanoseconds has no faithful NS
	// representation; the predicate must be treated as unrepresentable rather than
	// silently clamped into a wrong (row-dropping) window.
	auto filter = make_uniq<ConstantFilter>(ExpressionType::COMPARE_GREATERTHAN,
	                                        Value::TIMESTAMP(timestamp_t(9000000000000000000LL)));
	mcap::Timestamp start = 0;
	mcap::Timestamp end = 0;
	REQUIRE_FALSE(CollectTimeRange(start, end, *filter));
}

TEST_CASE("timestamp bound bump never inverts at the int64 ceiling", "[pushdown][timestamp]") {
	// '>' on the largest representable NS timestamp: the +1ns bump must not wrap.
	auto max_ns = std::numeric_limits<int64_t>::max();
	auto filter =
	    make_uniq<ConstantFilter>(ExpressionType::COMPARE_GREATERTHAN, Value::TIMESTAMPNS(timestamp_ns_t(max_ns)));
	mcap::Timestamp start = 0;
	mcap::Timestamp end = 0;
	REQUIRE(CollectTimeRange(start, end, *filter));
	REQUIRE(start == static_cast<mcap::Timestamp>(max_ns) + 1);
	REQUIRE(end == mcap::MaxTime);
	REQUIRE(start <= end);
}
