#include "pushdown.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "mcap_scan.hpp"

namespace duckdb {

static mcap::Timestamp TimestampValueToNanos(const Value &value) {
	auto timestamp = value.GetValue<timestamp_t>();
	if (timestamp.value < 0) {
		return 0;
	}
	return static_cast<mcap::Timestamp>(timestamp.value) * 1000;
}

static void AddTopic(McapPushdown &pushdown, const Value &value) {
	if (!pushdown.topics.has_value()) {
		pushdown.topics.emplace();
	}
	pushdown.topics->insert(value.ToString());
}

static void ApplyTopicFilter(McapPushdown &pushdown, const TableFilter &filter);
static void ApplyTimestampFilter(McapPushdown &pushdown, const TableFilter &filter);

static void ApplyConjunction(McapPushdown &pushdown, const TableFilter &filter, idx_t column_index) {
	auto apply_child = [&](const unique_ptr<TableFilter> &child) {
		if (column_index == static_cast<idx_t>(McapScanColumn::TOPIC)) {
			ApplyTopicFilter(pushdown, *child);
		} else if (column_index == static_cast<idx_t>(McapScanColumn::TIMESTAMP)) {
			ApplyTimestampFilter(pushdown, *child);
		}
	};

	if (filter.filter_type == TableFilterType::CONJUNCTION_AND) {
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		for (auto &child : conjunction.child_filters) {
			apply_child(child);
		}
	} else if (filter.filter_type == TableFilterType::CONJUNCTION_OR) {
		auto &conjunction = filter.Cast<ConjunctionOrFilter>();
		for (auto &child : conjunction.child_filters) {
			apply_child(child);
		}
	}
}

static void ApplyTopicFilter(McapPushdown &pushdown, const TableFilter &filter) {
	if (filter.filter_type == TableFilterType::CONSTANT_COMPARISON) {
		auto &constant = filter.Cast<ConstantFilter>();
		if (constant.comparison_type == ExpressionType::COMPARE_EQUAL) {
			AddTopic(pushdown, constant.constant);
		}
		return;
	}
	if (filter.filter_type == TableFilterType::IN_FILTER) {
		auto &in_filter = filter.Cast<InFilter>();
		for (auto &value : in_filter.values) {
			AddTopic(pushdown, value);
		}
		return;
	}
	ApplyConjunction(pushdown, filter, static_cast<idx_t>(McapScanColumn::TOPIC));
}

static void ApplyTimestampFilter(McapPushdown &pushdown, const TableFilter &filter) {
	if (filter.filter_type == TableFilterType::CONSTANT_COMPARISON) {
		auto &constant = filter.Cast<ConstantFilter>();
		auto nanos = TimestampValueToNanos(constant.constant);
		switch (constant.comparison_type) {
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
		return;
	}
	ApplyConjunction(pushdown, filter, static_cast<idx_t>(McapScanColumn::TIMESTAMP));
}

bool McapSupportsPushdown(const FunctionData &, idx_t column_index) {
	return column_index == static_cast<idx_t>(McapScanColumn::TOPIC) ||
	       column_index == static_cast<idx_t>(McapScanColumn::TIMESTAMP);
}

McapPushdown ExtractMcapPushdown(const TableFunctionInitInput &input) {
	McapPushdown result;
	if (!input.filters) {
		return result;
	}
	for (auto &entry : input.filters->filters) {
		if (entry.first == static_cast<idx_t>(McapScanColumn::TOPIC)) {
			ApplyTopicFilter(result, *entry.second);
		} else if (entry.first == static_cast<idx_t>(McapScanColumn::TIMESTAMP)) {
			ApplyTimestampFilter(result, *entry.second);
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
