#include "mcap_metadata.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"

#include <mcap/reader.hpp>

namespace duckdb {

namespace {

static void ThrowIfMcapError(const mcap::Status &status, const string &path) {
	if (!status.ok()) {
		throw IOException("Failed to read MCAP file '%s': %s", path, status.message);
	}
}

struct McapSchemasBindData : public TableFunctionData {
	explicit McapSchemasBindData(string path_p) : path(std::move(path_p)) {
	}

	string path;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<McapSchemasBindData>(path);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<McapSchemasBindData>();
		return path == other.path;
	}
};

struct SchemaRow {
	uint16_t id;
	string name;
	string encoding;
	string data;
};

struct SchemasGlobalState : public GlobalTableFunctionState {
	vector<SchemaRow> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> SchemasBind(ClientContext &, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("mcap_schemas(path) requires a single non-null path argument");
	}
	names.emplace_back("id");
	return_types.emplace_back(LogicalTypeId::USMALLINT);
	names.emplace_back("name");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("encoding");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("data");
	return_types.emplace_back(LogicalTypeId::BLOB);
	return make_uniq<McapSchemasBindData>(input.inputs[0].GetValue<string>());
}

static unique_ptr<GlobalTableFunctionState> SchemasInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<McapSchemasBindData>();
	auto result = make_uniq<SchemasGlobalState>();

	mcap::McapReader reader;
	ThrowIfMcapError(reader.open(bind.path), bind.path);
	ThrowIfMcapError(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan), bind.path);

	for (auto &entry : reader.schemas()) {
		auto schema = entry.second;
		auto data = string(reinterpret_cast<const char *>(schema->data.data()), schema->data.size());
		result->rows.push_back({entry.first, schema->name, schema->encoding, std::move(data)});
	}
	return std::move(result);
}

static void SchemasScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SchemasGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.rows.size()) {
		auto &row = state.rows[state.offset++];
		output.SetValue(0, count, Value::USMALLINT(row.id));
		output.SetValue(1, count, Value(row.name));
		output.SetValue(2, count, Value(row.encoding));
		FlatVector::GetData<string_t>(output.data[3])[count] =
		    StringVector::AddString(output.data[3], row.data.data(), row.data.size());
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

TableFunction GetMcapSchemasFunction() {
	return TableFunction("mcap_schemas", {LogicalTypeId::VARCHAR}, SchemasScan, SchemasBind, SchemasInitGlobal);
}

} // namespace duckdb
