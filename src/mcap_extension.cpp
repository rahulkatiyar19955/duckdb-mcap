#define DUCKDB_EXTENSION_MAIN

#include "mcap_extension.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "mcap_metadata.hpp"
#include "mcap_scan.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GetMcapScanFunction());
	loader.RegisterFunction(GetMcapTopicsFunction());
	loader.RegisterFunction(GetMcapSchemasFunction());
	loader.RegisterFunction(GetMcapChannelsFunction());
	loader.RegisterFunction(GetMcapInfoFunction());
	loader.RegisterFunction(GetMcapAttachmentsFunction());
	loader.RegisterFunction(GetMcapMetadataFunction());
	loader.RegisterFunction(GetRosoutFunction());
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
