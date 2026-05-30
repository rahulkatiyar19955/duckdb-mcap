#include "pushdown.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "mcap_scan.hpp"

namespace duckdb {

static mcap::Timestamp TimestampValueToNanos(const Value &value) {
	auto timestamp = value.GetValue<timestamp_t>();
	if (timestamp.value < 0) {
		return 0;
	}
	return static_cast<mcap::Timestamp>(timestamp.value) * 1000;
}

//===--------------------------------------------------------------------===//
// Topic filters
//
// DuckDB 1.5 wraps simple predicates (=, IN, OR, LIKE, ...) in an
// EXPRESSION_FILTER. Rather than special-casing every shape, we resolve topic
// predicates by evaluating each known topic in the file against the filter and
// keeping those that pass. This is exact and handles all predicate forms.
//===--------------------------------------------------------------------===//

static void AddTopic(McapPushdown &pushdown, const string &topic) {
	if (!pushdown.topics.has_value()) {
		pushdown.topics.emplace();
	}
	pushdown.topics->insert(topic);
}

static void ApplyTopicFilter(ClientContext &context, McapPushdown &pushdown, const TableFilter &filter,
                             const vector<string> &known_topics) {
	// Fast paths for the legacy filter shapes (still emitted in some plans).
	if (filter.filter_type == TableFilterType::LEGACY_CONSTANT_COMPARISON) {
		auto &constant = filter.Cast<LegacyConstantFilter>();
		if (constant.comparison_type == ExpressionType::COMPARE_EQUAL) {
			AddTopic(pushdown, constant.constant.ToString());
		}
		return;
	}
	if (filter.filter_type == TableFilterType::LEGACY_IN_FILTER) {
		auto &in_filter = filter.Cast<LegacyInFilter>();
		for (auto &value : in_filter.values) {
			AddTopic(pushdown, value.ToString());
		}
		return;
	}

	// General path: evaluate every known topic against the filter expression.
	auto expr_filter = ExpressionFilter::FromTableFilter(filter, LogicalType::VARCHAR);
	if (!expr_filter) {
		return;
	}
	// Always materialize the set: DuckDB pushes the filter fully into this scan and
	// does NOT re-check it, so an empty match-set must mean "read nothing" (e.g. a
	// predicate that matches no existing topic) rather than "no filter".
	if (!pushdown.topics.has_value()) {
		pushdown.topics.emplace();
	}
	for (auto &topic : known_topics) {
		if (expr_filter->EvaluateWithConstant(context, Value(topic))) {
			pushdown.topics->insert(topic);
		}
	}
}

//===--------------------------------------------------------------------===//
// Timestamp filters
//
// We walk the bound expression tree of an EXPRESSION_FILTER (and the legacy
// constant/conjunction shapes) to derive a [start, end) nanosecond window that
// is forwarded to MCAP's index-based chunk pruning.
//===--------------------------------------------------------------------===//

static void NarrowRange(McapPushdown &pushdown, ExpressionType comparison, const Value &constant) {
	if (constant.IsNull()) {
		return;
	}
	auto nanos = TimestampValueToNanos(constant);
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
		pushdown.start_time = std::max(pushdown.start_time, nanos);
		pushdown.end_time = std::min(pushdown.end_time, nanos + 1000);
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		pushdown.start_time = std::max(pushdown.start_time, nanos + 1000);
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		pushdown.start_time = std::max(pushdown.start_time, nanos);
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		pushdown.end_time = std::min(pushdown.end_time, nanos);
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		pushdown.end_time = std::min(pushdown.end_time, nanos + 1000);
		break;
	default:
		break;
	}
}

static ExpressionType FlipComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_GREATERTHAN:
		return ExpressionType::COMPARE_LESSTHAN;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExpressionType::COMPARE_LESSTHANOREQUALTO;
	case ExpressionType::COMPARE_LESSTHAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	default:
		return type;
	}
}

static void WalkTimestampExpression(McapPushdown &pushdown, const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// Only AND lets us safely intersect ranges; OR is left to DuckDB's filter.
		if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			for (auto &child : conj.children) {
				WalkTimestampExpression(pushdown, *child);
			}
		}
		break;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (!BoundComparisonExpression::IsComparison(func.GetExpressionType()) || func.children.size() != 2) {
			break;
		}
		auto &lhs = *func.children[0];
		auto &rhs = *func.children[1];
		// One side must be the timestamp column ref, the other a constant.
		if (lhs.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			NarrowRange(pushdown, FlipComparison(func.GetExpressionType()),
			            lhs.Cast<BoundConstantExpression>().value);
		} else if (rhs.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			NarrowRange(pushdown, func.GetExpressionType(), rhs.Cast<BoundConstantExpression>().value);
		}
		break;
	}
	default:
		break;
	}
}

static void ApplyTimestampFilter(McapPushdown &pushdown, const TableFilter &filter) {
	if (filter.filter_type == TableFilterType::LEGACY_CONSTANT_COMPARISON) {
		auto &constant = filter.Cast<LegacyConstantFilter>();
		NarrowRange(pushdown, constant.comparison_type, constant.constant);
		return;
	}
	if (filter.filter_type == TableFilterType::LEGACY_CONJUNCTION_AND) {
		auto &conjunction = filter.Cast<LegacyConjunctionAndFilter>();
		for (auto &child : conjunction.child_filters) {
			ApplyTimestampFilter(pushdown, *child);
		}
		return;
	}
	if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
		auto &expr_filter = filter.Cast<ExpressionFilter>();
		if (expr_filter.expr) {
			WalkTimestampExpression(pushdown, *expr_filter.expr);
		}
	}
}

bool McapSupportsPushdown(const FunctionData &, idx_t column_index) {
	return column_index == static_cast<idx_t>(McapScanColumn::TOPIC) ||
	       column_index == static_cast<idx_t>(McapScanColumn::TIMESTAMP);
}

McapPushdown ExtractMcapPushdown(ClientContext &context, const TableFunctionInitInput &input,
                                 const vector<string> &known_topics) {
	McapPushdown result;
	if (!input.filters) {
		return result;
	}
	for (auto &entry : *input.filters) {
		// Filter keys are indices into the projected column_ids list, not logical
		// table columns; translate back to the real column id before matching.
		auto filter_index = static_cast<idx_t>(entry.GetIndex());
		if (filter_index >= input.column_ids.size()) {
			continue;
		}
		auto column_index = input.column_ids[filter_index];
		if (column_index == static_cast<idx_t>(McapScanColumn::TOPIC)) {
			ApplyTopicFilter(context, result, entry.Filter(), known_topics);
		} else if (column_index == static_cast<idx_t>(McapScanColumn::TIMESTAMP)) {
			ApplyTimestampFilter(result, entry.Filter());
		}
	}
	return result;
}

mcap::ReadMessageOptions ToReadMessageOptions(const McapPushdown &pushdown) {
	mcap::ReadMessageOptions options;
	options.startTime = pushdown.start_time;
	options.endTime = pushdown.end_time;
	if (pushdown.topics.has_value()) {
		auto topics = *pushdown.topics;
		options.topicFilter = [topics = std::move(topics)](std::string_view topic) {
			return topics.find(std::string(topic)) != topics.end();
		};
	}
	return options;
}

} // namespace duckdb
