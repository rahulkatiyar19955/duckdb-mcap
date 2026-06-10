#include "mcap_metadata.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "mcap_file.hpp"
#include "mcap_time.hpp"

#include <mcap/reader.hpp>

namespace duckdb {

namespace {

struct McapRecordsBindData : public TableFunctionData {
	explicit McapRecordsBindData(string path_p) : path(std::move(path_p)) {
	}

	string path;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<McapRecordsBindData>(path);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<McapRecordsBindData>();
		return path == other.path;
	}
};

//! Generic "materialize rows of Values at init, emit by offset" state.
struct ValueRowsGlobalState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static void ValueRowsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ValueRowsGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.rows.size()) {
		auto &row = state.rows[state.offset++];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		count++;
	}
	output.SetCardinality(count);
}

static unique_ptr<FunctionData> SinglePathBind(TableFunctionBindInput &input, const char *function_name) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("%s(path) requires a single non-null path argument", function_name);
	}
	return make_uniq<McapRecordsBindData>(input.inputs[0].GetValue<string>());
}

//===--------------------------------------------------------------------===//
// mcap_info: one row of header + statistics
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> InfoBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("profile");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("library");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("message_count");
	return_types.emplace_back(LogicalTypeId::UBIGINT);
	names.emplace_back("channel_count");
	return_types.emplace_back(LogicalTypeId::UINTEGER);
	names.emplace_back("schema_count");
	return_types.emplace_back(LogicalTypeId::UINTEGER);
	names.emplace_back("chunk_count");
	return_types.emplace_back(LogicalTypeId::UINTEGER);
	names.emplace_back("attachment_count");
	return_types.emplace_back(LogicalTypeId::UINTEGER);
	names.emplace_back("metadata_count");
	return_types.emplace_back(LogicalTypeId::UINTEGER);
	names.emplace_back("start");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP_NS);
	names.emplace_back("end");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP_NS);
	return SinglePathBind(input, "mcap_info");
}

static unique_ptr<GlobalTableFunctionState> InfoInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<McapRecordsBindData>();
	auto result = make_uniq<ValueRowsGlobalState>();

	auto file = OpenMcapFile(context, bind.path);
	auto &reader = file->reader;

	auto &header = reader.header();
	auto &stats = reader.statistics();
	vector<Value> row(10);
	if (header.has_value()) {
		row[0] = Value(header->profile);
		row[1] = Value(header->library);
	}
	if (stats.has_value()) {
		row[2] = Value::UBIGINT(stats->messageCount);
		row[3] = Value::UINTEGER(stats->channelCount);
		row[4] = Value::UINTEGER(stats->schemaCount);
		row[5] = Value::UINTEGER(stats->chunkCount);
		row[6] = Value::UINTEGER(stats->attachmentCount);
		row[7] = Value::UINTEGER(stats->metadataCount);
		if (stats->messageCount > 0) {
			row[8] = Value::TIMESTAMPNS(ToTimestampNs(stats->messageStartTime));
			row[9] = Value::TIMESTAMPNS(ToTimestampNs(stats->messageEndTime));
		}
	}
	result->rows.push_back(std::move(row));
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// mcap_attachments: one row per attachment record
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> AttachmentsBind(ClientContext &, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("media_type");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("log_time");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP_NS);
	names.emplace_back("create_time");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP_NS);
	names.emplace_back("data_size");
	return_types.emplace_back(LogicalTypeId::UBIGINT);
	names.emplace_back("data");
	return_types.emplace_back(LogicalTypeId::BLOB);
	return SinglePathBind(input, "mcap_attachments");
}

static unique_ptr<GlobalTableFunctionState> AttachmentsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<McapRecordsBindData>();
	auto result = make_uniq<ValueRowsGlobalState>();

	auto file = OpenMcapFile(context, bind.path);
	auto &reader = file->reader;

	for (auto &entry : reader.attachmentIndexes()) {
		auto &index = entry.second;
		mcap::RecordReader record_reader(*reader.dataSource(), index.offset);
		auto record = record_reader.next();
		if (!record.has_value() || !record_reader.status().ok()) {
			continue;
		}
		mcap::Attachment attachment;
		if (!mcap::McapReader::ParseAttachment(*record, &attachment).ok()) {
			continue;
		}
		vector<Value> row(6);
		row[0] = Value(attachment.name);
		row[1] = Value(attachment.mediaType);
		row[2] = Value::TIMESTAMPNS(ToTimestampNs(attachment.logTime));
		row[3] = Value::TIMESTAMPNS(ToTimestampNs(attachment.createTime));
		row[4] = Value::UBIGINT(attachment.dataSize);
		// Copy the payload: attachment.data points into the record reader's buffer.
		row[5] = Value::BLOB(reinterpret_cast<const uint8_t *>(attachment.data),
		                     static_cast<idx_t>(attachment.dataSize));
		result->rows.push_back(std::move(row));
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// mcap_metadata: one row per key/value pair per metadata record
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MetadataBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("key");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("value");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	return SinglePathBind(input, "mcap_metadata");
}

static unique_ptr<GlobalTableFunctionState> MetadataInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<McapRecordsBindData>();
	auto result = make_uniq<ValueRowsGlobalState>();

	auto file = OpenMcapFile(context, bind.path);
	auto &reader = file->reader;

	for (auto &entry : reader.metadataIndexes()) {
		auto &index = entry.second;
		mcap::RecordReader record_reader(*reader.dataSource(), index.offset);
		auto record = record_reader.next();
		if (!record.has_value() || !record_reader.status().ok()) {
			continue;
		}
		mcap::Metadata metadata;
		if (!mcap::McapReader::ParseMetadata(*record, &metadata).ok()) {
			continue;
		}
		for (auto &kv : metadata.metadata) {
			result->rows.push_back({Value(metadata.name), Value(kv.first), Value(kv.second)});
		}
	}
	return std::move(result);
}

} // namespace

TableFunction GetMcapInfoFunction() {
	return TableFunction("mcap_info", {LogicalTypeId::VARCHAR}, ValueRowsScan, InfoBind, InfoInitGlobal);
}

TableFunction GetMcapAttachmentsFunction() {
	return TableFunction("mcap_attachments", {LogicalTypeId::VARCHAR}, ValueRowsScan, AttachmentsBind,
	                     AttachmentsInitGlobal);
}

TableFunction GetMcapMetadataFunction() {
	return TableFunction("mcap_metadata", {LogicalTypeId::VARCHAR}, ValueRowsScan, MetadataBind, MetadataInitGlobal);
}

} // namespace duckdb
