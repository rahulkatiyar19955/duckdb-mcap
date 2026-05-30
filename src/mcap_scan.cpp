#include "mcap_scan.hpp"

#include "decoder.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/main/client_context.hpp"
#include "pushdown.hpp"
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
	mcap::McapReader reader;
	McapSchemaCache cache;
	unique_ptr<mcap::LinearMessageView> view;
	std::optional<mcap::LinearMessageView::Iterator> iterator;
	std::optional<mcap::LinearMessageView::Iterator> end;
	vector<column_t> column_ids;
	McapPushdown pushdown;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static timestamp_t ToDuckTimestamp(mcap::Timestamp timestamp_ns) {
	return Timestamp::FromEpochMicroSeconds(static_cast<int64_t>(timestamp_ns / 1000));
}

static void ThrowIfMcapError(const mcap::Status &status, const string &path) {
	if (!status.ok()) {
		throw IOException("Failed to read MCAP file '%s': %s", path, status.message);
	}
}

static unique_ptr<FunctionData> McapScanBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("mcap_scan(path) requires a single non-null path argument");
	}

	auto path = input.inputs[0].GetValue<string>();

	mcap::McapReader reader;
	ThrowIfMcapError(reader.open(path), path);
	reader.close();

	names.emplace_back("timestamp");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("topic");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("schema");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("payload_blob");
	return_types.emplace_back(LogicalType::BLOB);
	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("payload_json");
	return_types.emplace_back(LogicalType::JSON());

	return make_uniq<McapScanBindData>(std::move(path));
}

static unique_ptr<GlobalTableFunctionState> McapScanInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<McapScanBindData>();
	auto result = make_uniq<McapScanGlobalState>();
	result->column_ids = input.column_ids;
	if (result->column_ids.empty()) {
		for (idx_t i = 0; i < MCAP_SCAN_COLUMN_COUNT; i++) {
			result->column_ids.push_back(i);
		}
	}

	ThrowIfMcapError(result->reader.open(bind.path), bind.path);

	result->pushdown = ExtractMcapPushdown(input);
	if (result->pushdown.start_time != 0 || result->pushdown.end_time != mcap::MaxTime) {
		ThrowIfMcapError(result->reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan), bind.path);
	}
	result->cache = McapSchemaCache::FromReader(result->reader);

	auto options = ToReadMessageOptions(result->pushdown);
	auto on_problem = [](const mcap::Status &status) {
		if (!status.ok()) {
			throw IOException("MCAP scan failed: %s", status.message);
		}
	};
	result->view = make_uniq<mcap::LinearMessageView>(result->reader.readMessages(on_problem, options));
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
		FlatVector::GetDataMutable<timestamp_t>(vector)[row] = ToDuckTimestamp(message_view.message.logTime);
		break;
	case McapScanColumn::TOPIC:
		FlatVector::GetDataMutable<string_t>(vector)[row] = StringVector::AddString(vector, message_view.channel->topic);
		break;
	case McapScanColumn::SCHEMA:
	case McapScanColumn::SCHEMA_NAME: {
		auto schema_name = channel_info ? channel_info->schema_name : string();
		FlatVector::GetDataMutable<string_t>(vector)[row] = StringVector::AddString(vector, schema_name);
		break;
	}
	case McapScanColumn::PAYLOAD_BLOB: {
		auto data = reinterpret_cast<const char *>(message_view.message.data);
		auto size = static_cast<size_t>(message_view.message.dataSize);
		FlatVector::GetDataMutable<string_t>(vector)[row] = StringVector::AddString(vector, data, size);
		break;
	}
	case McapScanColumn::PAYLOAD_JSON:
		if (!json_payload.has_value()) {
			FlatVector::SetNull(vector, row, true);
		} else {
			FlatVector::GetDataMutable<string_t>(vector)[row] = StringVector::AddString(vector, *json_payload);
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
		const auto &message_view = **state.iterator;
		++(*state.iterator);

		const auto *channel_info = state.cache.Lookup(message_view.message.channelId);
		std::optional<std::string> json_payload;
		if (decode_json) {
			auto encoding = channel_info ? channel_info->message_encoding : string();
			json_payload = DecodePayloadJson(encoding, message_view.message);
		}

		for (idx_t output_col = 0; output_col < state.column_ids.size(); output_col++) {
			WriteProjectedColumn(output, output_col, count, state.column_ids[output_col], message_view, channel_info,
			                     json_payload);
		}
		count++;
	}

	output.SetCardinality(count);
}

} // namespace

TableFunction GetMcapScanFunction() {
	TableFunction function("mcap_scan", {LogicalType::VARCHAR}, McapScanFunction, McapScanBind, McapScanInitGlobal,
	                       McapScanInitLocal);
	function.projection_pushdown = true;
	function.filter_pushdown = true;
	function.filter_prune = true;
	function.supports_pushdown_type = McapSupportsPushdown;
	return function;
}

} // namespace duckdb
