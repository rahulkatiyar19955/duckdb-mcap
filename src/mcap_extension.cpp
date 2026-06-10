#define DUCKDB_EXTENSION_MAIN

#include "mcap_extension.hpp"

#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "mcap_metadata.hpp"
#include "mcap_scan.hpp"

namespace duckdb {

//! Allow `FROM 'file.mcap'` to resolve to mcap_scan('file.mcap').
static unique_ptr<TableRef> McapReplacementScan(ClientContext &, ReplacementScanInput &input,
                                                optional_ptr<ReplacementScanData>) {
	auto table_name = ReplacementScan::GetFullPath(input);
	if (!ReplacementScan::CanReplace(table_name, {"mcap"})) {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>("mcap_scan", std::move(children));
	return std::move(table_function);
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GetMcapScanFunction());
	loader.RegisterFunction(GetMcapTopicsFunction());
	loader.RegisterFunction(GetMcapSchemasFunction());
	loader.RegisterFunction(GetMcapChannelsFunction());
	loader.RegisterFunction(GetMcapInfoFunction());
	loader.RegisterFunction(GetMcapAttachmentsFunction());
	loader.RegisterFunction(GetMcapMetadataFunction());
	loader.RegisterFunction(GetRosoutFunction());

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(McapReplacementScan);
}

void McapExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string McapExtension::Name() {
	return "mcap";
}

std::string McapExtension::Version() const {
#ifdef EXT_VERSION_MCAP
	return EXT_VERSION_MCAP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(mcap, loader) {
	duckdb::LoadInternal(loader);
}
}
