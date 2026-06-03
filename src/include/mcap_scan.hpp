#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

enum class McapScanColumn : idx_t {
	TIMESTAMP = 0,
	TOPIC = 1,
	PAYLOAD_BLOB = 2,
	SCHEMA_NAME = 3,
	PAYLOAD_JSON = 4
};

constexpr idx_t MCAP_SCAN_COLUMN_COUNT = 5;

TableFunction GetMcapScanFunction();

} // namespace duckdb
