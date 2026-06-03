#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/function/table_function.hpp"

#include <mcap/reader.hpp>

#include <optional>
#include <string>
#include <unordered_set>

namespace duckdb {

struct McapPushdown {
	std::optional<std::unordered_set<std::string>> topics;
	mcap::Timestamp start_time = 0;
	mcap::Timestamp end_time = mcap::MaxTime;

	bool HasTopicFilter() const {
		return topics.has_value();
	}
};

bool McapSupportsPushdown(const FunctionData &bind_data, idx_t column_index);
//! Extract MCAP read options from DuckDB's pushed-down filters (topic equality/IN,
//! timestamp range). Best-effort I/O pruning; DuckDB re-checks filters for exactness.
McapPushdown ExtractMcapPushdown(const TableFunctionInitInput &input);
mcap::ReadMessageOptions ToReadMessageOptions(const McapPushdown &pushdown);

class TableFilter;
//! Translate a single column's pushed-down filter into a scan constraint. Both are a
//! CONSERVATIVE over-approximation: a `true` result means the out-parameter is a
//! guaranteed SUPERSET of the rows matching `filter`; a `false` result means the
//! predicate could not be bounded and the scan must not be restricted on that column.
//! Declared here (rather than file-local) so they can be unit tested directly against
//! synthetic TableFilter trees.
bool CollectTopicSet(std::unordered_set<std::string> &out, const TableFilter &filter);
bool CollectTimeRange(mcap::Timestamp &start, mcap::Timestamp &end, const TableFilter &filter);

} // namespace duckdb
