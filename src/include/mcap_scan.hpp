#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

enum class McapScanColumn : idx_t {
	TIMESTAMP = 0,
	TOPIC = 1,
	SCHEMA = 2,
	PAYLOAD_BLOB = 3,
	SCHEMA_NAME = 4,
	PAYLOAD_JSON = 5
};

constexpr idx_t MCAP_SCAN_COLUMN_COUNT = 6;

TableFunction GetMcapScanFunction();

} // namespace duckdb
