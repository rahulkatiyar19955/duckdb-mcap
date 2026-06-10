#include "mcap_file.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>

namespace duckdb {

uint64_t DuckDBFileReadable::read(std::byte **output, uint64_t offset, uint64_t size) {
	if (offset >= file_size) {
		return 0;
	}
	auto to_read = std::min<uint64_t>(size, file_size - offset);
	try {
		buffer.resize(to_read);
		handle->Read(buffer.data(), to_read, offset);
	} catch (...) {
		return 0; // mcap contract: 0 signals a failed read
	}
	*output = buffer.data();
	return to_read;
}

unique_ptr<McapFile> OpenMcapFile(ClientContext &context, const std::string &path) {
	auto result = make_uniq<McapFile>();
	auto &fs = FileSystem::GetFileSystem(context);
	result->readable = make_uniq<DuckDBFileReadable>(fs, path); // throws IOException if missing
	auto status = result->reader.open(*result->readable);
	if (!status.ok()) {
		throw IOException("Failed to read MCAP file '%s': %s", path, status.message);
	}
	status = result->reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
	if (!status.ok()) {
		throw IOException("Failed to read MCAP file '%s': %s", path, status.message);
	}
	return result;
}

} // namespace duckdb
