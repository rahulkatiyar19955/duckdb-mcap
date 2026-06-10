#include "mcap_metadata.hpp"

#include "decoder.hpp"
#include "json_util.hpp"
#include "mcap_file.hpp"
#include "mcap_time.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "ros2_decoder.hpp"
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
	unique_ptr<McapFile> file;
	McapSchemaCache cache;
	Ros2Decoder ros2;
	unique_ptr<mcap::LinearMessageView> view;
	std::optional<mcap::LinearMessageView::Iterator> iterator;
	std::optional<mcap::LinearMessageView::Iterator> end;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> RosoutBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("rosout(path) requires a single non-null path argument");
	}

	names.emplace_back("timestamp");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP_NS);
	names.emplace_back("severity");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("node");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("message");
	return_types.emplace_back(LogicalTypeId::VARCHAR);

	return make_uniq<RosoutBindData>(input.inputs[0].GetValue<string>());
}

static unique_ptr<GlobalTableFunctionState> RosoutInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<RosoutBindData>();
	auto result = make_uniq<RosoutGlobalState>();

	result->file = OpenMcapFile(context, bind.path);
	result->cache = McapSchemaCache::FromReader(result->file->reader);

	mcap::ReadMessageOptions options;
	options.topicFilter = [](std::string_view topic) {
		return topic == "/rosout" || topic == "rosout" || topic == "/rosout_agg";
	};
	auto on_problem = [](const mcap::Status &status) {
		if (!status.ok()) {
			throw IOException("MCAP rosout scan failed: %s", status.message);
		}
	};
	result->view = make_uniq<mcap::LinearMessageView>(result->file->reader.readMessages(on_problem, options));
	result->iterator.emplace(result->view->begin());
	result->end.emplace(result->view->end());
	return std::move(result);
}

//! Map rcl_interfaces/msg/Log numeric levels to names; pass anything else through.
static std::optional<std::string> MapSeverity(std::optional<std::string> raw) {
	if (!raw.has_value()) {
		return raw;
	}
	if (*raw == "10") {
		return std::string("DEBUG");
	}
	if (*raw == "20") {
		return std::string("INFO");
	}
	if (*raw == "30") {
		return std::string("WARN");
	}
	if (*raw == "40") {
		return std::string("ERROR");
	}
	if (*raw == "50") {
		return std::string("FATAL");
	}
	return raw;
}

static void SetOptionalString(Vector &vector, idx_t row, const std::optional<std::string> &value) {
	if (!value.has_value()) {
		FlatVector::SetNull(vector, row, true);
		return;
	}
	FlatVector::GetData<string_t>(vector)[row] = StringVector::AddString(vector, *value);
	FlatVector::SetNull(vector, row, false);
}

static void RosoutScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RosoutGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && *state.iterator != *state.end) {
		// message_view (and message.data) is only valid until the iterator advances.
		const auto &message_view = **state.iterator;

		auto channel_info = state.cache.Lookup(message_view.message.channelId);
		// Real ROS2 bags record /rosout as CDR (rcl_interfaces/msg/Log); Foxglove
		// style bags use JSON. Both decode here; protobuf rosout does not exist.
		std::optional<std::string> payload;
		if (channel_info) {
			payload = DecodePayloadJson(*channel_info, message_view.message, nullptr, &state.ros2);
		}

		auto &ts_vector = output.data[0];
		FlatVector::GetData<timestamp_t>(ts_vector)[count] = ToTimestampNs(message_view.message.logTime);
		FlatVector::SetNull(ts_vector, count, false);
		if (!payload.has_value()) {
			FlatVector::SetNull(output.data[1], count, true);
			FlatVector::SetNull(output.data[2], count, true);
			FlatVector::SetNull(output.data[3], count, true);
		} else {
			// JSON rosout uses "severity"; rcl_interfaces/msg/Log uses numeric "level".
			auto severity = ExtractJsonStringField(*payload, "severity");
			if (!severity.has_value()) {
				severity = ExtractJsonStringField(*payload, "level");
			}
			SetOptionalString(output.data[1], count, MapSeverity(std::move(severity)));
			auto node = ExtractJsonStringField(*payload, "node");
			if (!node.has_value()) {
				node = ExtractJsonStringField(*payload, "name");
			}
			SetOptionalString(output.data[2], count, node);
			auto message = ExtractJsonStringField(*payload, "message");
			if (!message.has_value()) {
				message = ExtractJsonStringField(*payload, "msg");
			}
			SetOptionalString(output.data[3], count, message);
		}
		count++;
		++(*state.iterator);
	}
	output.SetCardinality(count);
}

} // namespace

TableFunction GetRosoutFunction() {
	// NOTE: RosoutScan always writes all 4 columns by fixed index, so it does not
	// support projection pushdown (doing so shrinks the output chunk and overflows).
	TableFunction function("rosout", {LogicalTypeId::VARCHAR}, RosoutScan, RosoutBind, RosoutInitGlobal);
	return function;
}

} // namespace duckdb
