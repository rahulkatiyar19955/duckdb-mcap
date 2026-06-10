#pragma once

#include "duckdb/common/file_system.hpp"

#include <mcap/reader.hpp>

#include <vector>

namespace duckdb {

class ClientContext;

//! mcap::IReadable over a DuckDB FileHandle, so any filesystem registered with
//! DuckDB (local, httpfs/S3, ...) works as an MCAP source. Not thread-safe: the
//! internal buffer is reused across read() calls — one instance per reader.
class DuckDBFileReadable final : public mcap::IReadable {
public:
	DuckDBFileReadable(FileSystem &fs, const std::string &path);

	uint64_t size() const override {
		return file_size;
	}

	uint64_t read(std::byte **output, uint64_t offset, uint64_t size) override;

private:
	unique_ptr<FileHandle> handle;
	uint64_t file_size;
	std::vector<std::byte> buffer;
};

//! An opened MCAP file with its summary read: keeps the readable alive for the
//! reader's lifetime (the reader holds a pointer into it).
struct McapFile {
	// Declaration order matters: `reader` must be destroyed before `readable`.
	unique_ptr<DuckDBFileReadable> readable;
	mcap::McapReader reader;

	~McapFile() {
		reader.close();
	}
};

//! Open `path` through DuckDB's FileSystem and read the MCAP summary (channel and
//! schema registry, chunk/attachment/metadata indexes, statistics). Throws
//! IOException on open or parse failure.
unique_ptr<McapFile> OpenMcapFile(ClientContext &context, const std::string &path);

} // namespace duckdb
