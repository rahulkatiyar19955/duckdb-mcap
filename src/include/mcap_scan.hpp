#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

enum class McapScanColumn : idx_t {
	TIMESTAMP = 0,
	TOPIC = 1,
	PAYLOAD_BLOB = 2,
	SCHEMA_NAME = 3,
	PAYLOAD_JSON = 4,
	PUBLISH_TIME = 5,
	SEQUENCE = 6,
	CHANNEL_ID = 7
};

constexpr idx_t MCAP_SCAN_COLUMN_COUNT = 8;

TableFunction GetMcapScanFunction();

} // namespace duckdb
