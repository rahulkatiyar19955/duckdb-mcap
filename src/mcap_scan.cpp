#include "mcap_scan.hpp"

#include "decoder.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/main/client_context.hpp"
#include "mcap_file.hpp"
#include "mcap_time.hpp"
#include "protobuf_decoder.hpp"
#include "pushdown.hpp"
#include "ros2_decoder.hpp"
#include "schema_cache.hpp"

#include <mcap/reader.hpp>

#include <optional>

namespace duckdb {

namespace {

struct McapScanBindData : public TableFunctionData {
	explicit McapScanBindData(string path_p) : path(std::move(path_p)) {
	}

	string path;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<McapScanBindData>(path);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<McapScanBindData>();
		return path == other.path;
	}
};

struct McapScanGlobalState : public GlobalTableFunctionState {
	unique_ptr<McapFile> file;
	McapSchemaCache cache;
	unique_ptr<mcap::LinearMessageView> view;
	std::optional<mcap::LinearMessageView::Iterator> iterator;
	std::optional<mcap::LinearMessageView::Iterator> end;
	vector<column_t> column_ids;
	McapPushdown pushdown;
	ProtobufDecoder protobuf;
	Ros2Decoder ros2;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> McapScanBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("mcap_scan(path) requires a single non-null path argument");
	}

	auto path = input.inputs[0].GetValue<string>();
	OpenMcapFile(context, path); // validate the file is readable MCAP at bind time

	names.emplace_back("timestamp");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP_NS);
	names.emplace_back("topic");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("payload_blob");
	return_types.emplace_back(LogicalTypeId::BLOB);
	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("payload_json");
	return_types.emplace_back(LogicalType::JSON());
	names.emplace_back("publish_time");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP_NS);
	names.emplace_back("sequence");
	return_types.emplace_back(LogicalTypeId::UINTEGER);
	names.emplace_back("channel_id");
	return_types.emplace_back(LogicalTypeId::USMALLINT);

	return make_uniq<McapScanBindData>(std::move(path));
}

static unique_ptr<GlobalTableFunctionState> McapScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<McapScanBindData>();
	auto result = make_uniq<McapScanGlobalState>();
	result->column_ids = input.column_ids;
	if (result->column_ids.empty()) {
		for (idx_t i = 0; i < MCAP_SCAN_COLUMN_COUNT; i++) {
			result->column_ids.push_back(i);
		}
	}

	// OpenMcapFile reads the summary too: it populates the channel/schema registry
	// (needed for schema_name + JSON decoding) and enables index-based pruning.
	result->file = OpenMcapFile(context, bind.path);

	result->cache = McapSchemaCache::FromReader(result->file->reader);
	result->pushdown = ExtractMcapPushdown(input);

	auto options = ToReadMessageOptions(result->pushdown);
	auto on_problem = [](const mcap::Status &status) {
		if (!status.ok()) {
			throw IOException("MCAP scan failed: %s", status.message);
		}
	};
	result->view = make_uniq<mcap::LinearMessageView>(result->file->reader.readMessages(on_problem, options));
	result->iterator.emplace(result->view->begin());
	result->end.emplace(result->view->end());

	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> McapScanInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                             GlobalTableFunctionState *) {
	return make_uniq<LocalTableFunctionState>();
}

static bool NeedsJson(const McapScanGlobalState &state) {
	for (auto column_id : state.column_ids) {
		if (column_id == static_cast<column_t>(McapScanColumn::PAYLOAD_JSON)) {
			return true;
		}
	}
	return false;
}

static void WriteProjectedColumn(DataChunk &output, idx_t output_col, idx_t row, column_t column_id,
                                 const mcap::MessageView &message_view, const McapChannelInfo *channel_info,
                                 const std::optional<std::string> &json_payload) {
	auto &vector = output.data[output_col];
	switch (static_cast<McapScanColumn>(column_id)) {
	case McapScanColumn::TIMESTAMP:
		FlatVector::GetData<timestamp_t>(vector)[row] = ToTimestampNs(message_view.message.logTime);
		FlatVector::SetNull(vector, row, false);
		break;
	case McapScanColumn::PUBLISH_TIME:
		FlatVector::GetData<timestamp_t>(vector)[row] = ToTimestampNs(message_view.message.publishTime);
		FlatVector::SetNull(vector, row, false);
		break;
	case McapScanColumn::SEQUENCE:
		FlatVector::GetData<uint32_t>(vector)[row] = message_view.message.sequence;
		FlatVector::SetNull(vector, row, false);
		break;
	case McapScanColumn::CHANNEL_ID:
		FlatVector::GetData<uint16_t>(vector)[row] = message_view.message.channelId;
		FlatVector::SetNull(vector, row, false);
		break;
	case McapScanColumn::TOPIC:
		FlatVector::GetData<string_t>(vector)[row] = StringVector::AddString(vector, message_view.channel->topic);
		FlatVector::SetNull(vector, row, false);
		break;
	case McapScanColumn::SCHEMA_NAME: {
		auto schema_name = channel_info ? channel_info->schema_name : string();
		FlatVector::GetData<string_t>(vector)[row] = StringVector::AddString(vector, schema_name);
		FlatVector::SetNull(vector, row, false);
		break;
	}
	case McapScanColumn::PAYLOAD_BLOB: {
		auto data = reinterpret_cast<const char *>(message_view.message.data);
		auto size = static_cast<size_t>(message_view.message.dataSize);
		FlatVector::GetData<string_t>(vector)[row] = StringVector::AddString(vector, data, size);
		FlatVector::SetNull(vector, row, false);
		break;
	}
	case McapScanColumn::PAYLOAD_JSON:
		if (!json_payload.has_value()) {
			FlatVector::SetNull(vector, row, true);
		} else {
			FlatVector::GetData<string_t>(vector)[row] = StringVector::AddString(vector, *json_payload);
			FlatVector::SetNull(vector, row, false);
		}
		break;
	default:
		FlatVector::SetNull(vector, row, true);
		break;
	}
}

static void McapScanFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<McapScanGlobalState>();
	idx_t count = 0;
	auto decode_json = NeedsJson(state);

	while (count < STANDARD_VECTOR_SIZE && *state.iterator != *state.end) {
		// NOTE: message_view (and message.data) is only valid until the iterator is
		// advanced, so read everything we need before the ++ at the end of the loop.
		const auto &message_view = **state.iterator;

		const auto *channel_info = state.cache.Lookup(message_view.message.channelId);
		std::optional<std::string> json_payload;
		if (decode_json && channel_info) {
			json_payload = DecodePayloadJson(*channel_info, message_view.message, &state.protobuf, &state.ros2);
		}

		for (idx_t output_col = 0; output_col < state.column_ids.size(); output_col++) {
			WriteProjectedColumn(output, output_col, count, state.column_ids[output_col], message_view, channel_info,
			                     json_payload);
		}
		count++;
		++(*state.iterator);
	}

	output.SetCardinality(count);
}

} // namespace

TableFunction GetMcapScanFunction() {
	TableFunction function("mcap_scan", {LogicalTypeId::VARCHAR}, McapScanFunction, McapScanBind, McapScanInitGlobal,
	                       McapScanInitLocal);
	function.projection_pushdown = true;
	function.filter_pushdown = true;
	// Keep DuckDB's own filter evaluation: our topic/timestamp pushdown is an I/O
	// optimization (skip channels/chunks), not an exact row-level filter, so we let
	// the executor re-check predicates for correctness.
	function.filter_prune = false;
	function.supports_pushdown_type = McapSupportsPushdown;
	return function;
}

} // namespace duckdb
