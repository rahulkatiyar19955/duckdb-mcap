#include "pushdown.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "mcap_scan.hpp"

#include <algorithm>

namespace duckdb {

//! Convert a pushed-down filter constant to MCAP nanoseconds. The scan column is
//! TIMESTAMP_NS so constants normally arrive in nanoseconds already; anything else
//! is cast. Returns false when no faithful conversion exists (e.g. out-of-range or
//! infinity sentinels) — the caller must then leave the time window unrestricted.
static bool TimestampFilterValueToNanos(const Value &value, mcap::Timestamp &out) {
	if (value.IsNull()) {
		return false;
	}
	int64_t nanos;
	if (value.type().id() == LogicalTypeId::TIMESTAMP_NS) {
		nanos = TimestampNSValue::Get(value).value;
	} else {
		Value cast;
		string error;
		try {
			// DefaultTryCastAs can still throw for some cast pairs (e.g. out-of-range
			// US -> NS) instead of returning false; both outcomes mean "unbounded".
			if (!value.DefaultTryCastAs(LogicalType::TIMESTAMP_NS, cast, &error) || cast.IsNull()) {
				return false;
			}
		} catch (...) {
			return false;
		}
		nanos = TimestampNSValue::Get(cast).value;
	}
	out = nanos < 0 ? 0 : static_cast<mcap::Timestamp>(nanos);
	return true;
}

//! Saturating add so the +1ns bumps used to widen exclusive/equality bounds never
//! overflow mcap::MaxTime and wrap around to a tiny value (which would invert the range).
static mcap::Timestamp SatAddNanos(mcap::Timestamp t, mcap::Timestamp delta) {
	return (t > mcap::MaxTime - delta) ? mcap::MaxTime : t + delta;
}

//===--------------------------------------------------------------------===//
// Topic predicate -> finite set of topics (best-effort, conservative)
//===--------------------------------------------------------------------===//
// Fills `out` with a SUPERSET of the topics that can satisfy `filter` and returns
// true iff the predicate is fully representable as a finite topic set. On false the
// caller must NOT restrict topics — the predicate may match topics not in `out`.
bool CollectTopicSet(std::unordered_set<std::string> &out, const TableFilter &filter) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant = filter.Cast<ConstantFilter>();
		if (constant.comparison_type == ExpressionType::COMPARE_EQUAL && !constant.constant.IsNull()) {
			out.insert(constant.constant.ToString());
			return true;
		}
		return false; // <, >, <> ... are not a finite topic set
	}
	case TableFilterType::IN_FILTER: {
		auto &in_filter = filter.Cast<InFilter>();
		std::unordered_set<std::string> local;
		for (auto &value : in_filter.values) {
			if (value.IsNull()) {
				return false; // leave `out` untouched: not a finite set
			}
			local.insert(value.ToString());
		}
		out.insert(local.begin(), local.end());
		return true;
	}
	case TableFilterType::CONJUNCTION_AND: {
		// AND matches a SUBSET of each child, so the union of the children we can
		// represent is a safe superset. Representable if at least one child is.
		// Collect each child into a local set first so an un-representable child does
		// not leave stray topics in `out` (which would needlessly widen the scan).
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		bool any = false;
		for (auto &child : conjunction.child_filters) {
			std::unordered_set<std::string> child_topics;
			if (CollectTopicSet(child_topics, *child)) {
				out.insert(child_topics.begin(), child_topics.end());
				any = true;
			}
		}
		return any;
	}
	case TableFilterType::CONJUNCTION_OR: {
		// OR matches the UNION of its children; we can only bound it if EVERY child is
		// representable. One un-representable branch leaves the topic unconstrained, so
		// accumulate into a local set and merge only once all branches have succeeded.
		auto &conjunction = filter.Cast<ConjunctionOrFilter>();
		if (conjunction.child_filters.empty()) {
			return false;
		}
		std::unordered_set<std::string> local;
		for (auto &child : conjunction.child_filters) {
			if (!CollectTopicSet(local, *child)) {
				return false;
			}
		}
		out.insert(local.begin(), local.end());
		return true;
	}
	default:
		return false;
	}
}

//===--------------------------------------------------------------------===//
// Timestamp predicate -> single contiguous [start, end] range (best-effort)
//===--------------------------------------------------------------------===//
// Computes a range that CONTAINS every row matching `filter` and returns true iff
// the predicate is representable as one contiguous range. On false the caller must
// NOT restrict the time window.
bool CollectTimeRange(mcap::Timestamp &start, mcap::Timestamp &end, const TableFilter &filter) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant = filter.Cast<ConstantFilter>();
		mcap::Timestamp nanos;
		if (!TimestampFilterValueToNanos(constant.constant, nanos)) {
			return false;
		}
		start = 0;
		end = mcap::MaxTime;
		// MCAP's read window is [start, end): widen exclusive/equality bounds by 1ns.
		switch (constant.comparison_type) {
		case ExpressionType::COMPARE_EQUAL:
			start = nanos;
			end = SatAddNanos(nanos, 1);
			return true;
		case ExpressionType::COMPARE_GREATERTHAN:
			start = SatAddNanos(nanos, 1);
			return true;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			start = nanos;
			return true;
		case ExpressionType::COMPARE_LESSTHAN:
			end = nanos;
			return true;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			end = SatAddNanos(nanos, 1);
			return true;
		default:
			return false;
		}
	}
	case TableFilterType::CONJUNCTION_AND: {
		// AND narrows: intersect the children we can represent.
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		bool any = false;
		mcap::Timestamp lo = 0;
		mcap::Timestamp hi = mcap::MaxTime;
		for (auto &child : conjunction.child_filters) {
			mcap::Timestamp child_start;
			mcap::Timestamp child_end;
			if (CollectTimeRange(child_start, child_end, *child)) {
				lo = std::max(lo, child_start);
				hi = std::min(hi, child_end);
				any = true;
			}
		}
		if (!any) {
			return false;
		}
		start = lo;
		end = hi;
		return true;
	}
	case TableFilterType::CONJUNCTION_OR: {
		// OR widens: a disjunction of ranges is bounded by their hull, but only if
		// EVERY child is representable (else the disjunction is unbounded). This is the
		// key correctness guard: intersecting OR branches (the old behaviour) produced
		// an empty/inverted window for "ts < a OR ts > b" and silently dropped rows.
		auto &conjunction = filter.Cast<ConjunctionOrFilter>();
		if (conjunction.child_filters.empty()) {
			return false;
		}
		mcap::Timestamp lo = mcap::MaxTime;
		mcap::Timestamp hi = 0;
		for (auto &child : conjunction.child_filters) {
			mcap::Timestamp child_start;
			mcap::Timestamp child_end;
			if (!CollectTimeRange(child_start, child_end, *child)) {
				return false;
			}
			lo = std::min(lo, child_start);
			hi = std::max(hi, child_end);
		}
		start = lo;
		end = hi;
		return true;
	}
	default:
		return false;
	}
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
		// Filter keys are indices into the projected column_ids list; translate
		// back to the real column id before matching.
		auto filter_index = entry.first;
		if (filter_index >= input.column_ids.size()) {
			continue;
		}
		auto column_index = input.column_ids[filter_index];
		if (column_index == static_cast<idx_t>(McapScanColumn::TOPIC)) {
			// Only restrict topics if the whole predicate is representable as a set;
			// otherwise reading every topic and letting DuckDB filter stays correct.
			std::unordered_set<std::string> topics;
			if (CollectTopicSet(topics, *entry.second) && !topics.empty()) {
				if (result.topics.has_value()) {
					result.topics->insert(topics.begin(), topics.end());
				} else {
					result.topics = std::move(topics);
				}
			}
		} else if (column_index == static_cast<idx_t>(McapScanColumn::TIMESTAMP)) {
			mcap::Timestamp start = 0;
			mcap::Timestamp end = mcap::MaxTime;
			if (CollectTimeRange(start, end, *entry.second)) {
				result.start_time = std::max(result.start_time, start);
				result.end_time = std::min(result.end_time, end);
			}
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
