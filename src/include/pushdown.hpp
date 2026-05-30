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
//! Extract MCAP read options from DuckDB's pushed-down filters. `known_topics`
//! enumerates every topic in the file so topic predicates (=, IN, OR, LIKE, ...)
//! can be resolved by evaluating each candidate against the filter expression.
McapPushdown ExtractMcapPushdown(ClientContext &context, const TableFunctionInitInput &input,
                                 const vector<string> &known_topics);
mcap::ReadMessageOptions ToReadMessageOptions(const McapPushdown &pushdown);

} // namespace duckdb
