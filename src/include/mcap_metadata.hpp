#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

TableFunction GetMcapTopicsFunction();
TableFunction GetMcapSchemasFunction();
TableFunction GetMcapChannelsFunction();
TableFunction GetRosoutFunction();

} // namespace duckdb
