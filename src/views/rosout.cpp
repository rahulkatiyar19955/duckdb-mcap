#include "mcap_metadata.hpp"

#include "decoder.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "schema_cache.hpp"

#include <mcap/reader.hpp>

#include <optional>

namespace duckdb {

namespace {

struct RosoutBindData : public TableFunctionData {
	explicit RosoutBindData(string path_p) : path(std::move(path_p)) {
	}

	string path;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RosoutBindData>(path);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RosoutBindData>();
		return path == other.path;
	}
};

struct RosoutGlobalState : public GlobalTableFunctionState {
	mcap::McapReader reader;
	McapSchemaCache cache;
	unique_ptr<mcap::LinearMessageView> view;
	std::optional<mcap::LinearMessageView::Iterator> iterator;
	std::optional<mcap::LinearMessageView::Iterator> end;

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

static unique_ptr<FunctionData> RosoutBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("rosout(path) requires a single non-null path argument");
	}

	names.emplace_back("timestamp");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("severity");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("node");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("message");
	return_types.emplace_back(LogicalType::VARCHAR);

	return make_uniq<RosoutBindData>(input.inputs[0].GetValue<string>());
}

static unique_ptr<GlobalTableFunctionState> RosoutInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<RosoutBindData>();
	auto result = make_uniq<RosoutGlobalState>();

	ThrowIfMcapError(result->reader.open(bind.path), bind.path);
	ThrowIfMcapError(result->reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan), bind.path);
	result->cache = McapSchemaCache::FromReader(result->reader);

	mcap::ReadMessageOptions options;
	options.topicFilter = [](std::string_view topic) {
		return topic == "/rosout" || topic == "rosout" || topic == "/rosout_agg";
	};
	auto on_problem = [](const mcap::Status &status) {
		if (!status.ok()) {
			throw IOException("MCAP rosout scan failed: %s", status.message);
		}
	};
	result->view = make_uniq<mcap::LinearMessageView>(result->reader.readMessages(on_problem, options));
	result->iterator.emplace(result->view->begin());
	result->end.emplace(result->view->end());
	return std::move(result);
}

static void SetOptionalString(DataChunk &output, idx_t column, idx_t row, const std::optional<std::string> &value) {
	if (!value.has_value()) {
		FlatVector::SetNull(output.data[column], row, true);
		return;
	}
	output.SetValue(column, row, Value(*value));
}

static void RosoutScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RosoutGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && *state.iterator != *state.end) {
		const auto &message_view = **state.iterator;
		++(*state.iterator);

		auto channel_info = state.cache.Lookup(message_view.message.channelId);
		auto encoding = channel_info ? channel_info->message_encoding : string();
		auto payload = DecodePayloadJson(encoding, message_view.message);

		output.SetValue(0, count, Value::TIMESTAMP(ToDuckTimestamp(message_view.message.logTime)));
		if (!payload.has_value()) {
			FlatVector::SetNull(output.data[1], count, true);
			FlatVector::SetNull(output.data[2], count, true);
			FlatVector::SetNull(output.data[3], count, true);
		} else {
			SetOptionalString(output, 1, count, ExtractJsonStringField(*payload, "severity"));
			auto node = ExtractJsonStringField(*payload, "node");
			if (!node.has_value()) {
				node = ExtractJsonStringField(*payload, "name");
			}
			SetOptionalString(output, 2, count, node);
			auto message = ExtractJsonStringField(*payload, "message");
			if (!message.has_value()) {
				message = ExtractJsonStringField(*payload, "msg");
			}
			SetOptionalString(output, 3, count, message);
		}
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

TableFunction GetRosoutFunction() {
	TableFunction function("rosout", {LogicalType::VARCHAR}, RosoutScan, RosoutBind, RosoutInitGlobal);
	function.projection_pushdown = true;
	return function;
}

} // namespace duckdb
