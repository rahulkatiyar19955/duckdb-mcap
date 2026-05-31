#include "mcap_metadata.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "schema_cache.hpp"

#include <mcap/reader.hpp>

namespace duckdb {

namespace {

static timestamp_t ToDuckTimestamp(mcap::Timestamp timestamp_ns) {
	return Timestamp::FromEpochMicroSeconds(static_cast<int64_t>(timestamp_ns / 1000));
}

static void ThrowIfMcapError(const mcap::Status &status, const string &path) {
	if (!status.ok()) {
		throw IOException("Failed to read MCAP file '%s': %s", path, status.message);
	}
}

struct McapMetadataBindData : public TableFunctionData {
	explicit McapMetadataBindData(string path_p) : path(std::move(path_p)) {
	}

	string path;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<McapMetadataBindData>(path);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<McapMetadataBindData>();
		return path == other.path;
	}
};

struct TopicRow {
	string topic;
	string type;
	uint64_t count;
	timestamp_t start;
	timestamp_t end;
};

struct ChannelRow {
	uint16_t id;
	string topic;
	uint16_t schema_id;
	string message_encoding;
};

template <class T>
struct VectorGlobalState : public GlobalTableFunctionState {
	vector<T> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PathBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("MCAP metadata functions require a single non-null path argument");
	}
	names.emplace_back("topic");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("type");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("count");
	return_types.emplace_back(LogicalTypeId::UBIGINT);
	names.emplace_back("start");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP);
	names.emplace_back("end");
	return_types.emplace_back(LogicalTypeId::TIMESTAMP);
	return make_uniq<McapMetadataBindData>(input.inputs[0].GetValue<string>());
}

static unique_ptr<FunctionData> ChannelsBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("mcap_channels(path) requires a single non-null path argument");
	}
	names.emplace_back("id");
	return_types.emplace_back(LogicalTypeId::USMALLINT);
	names.emplace_back("topic");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	names.emplace_back("schema_id");
	return_types.emplace_back(LogicalTypeId::USMALLINT);
	names.emplace_back("message_encoding");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	return make_uniq<McapMetadataBindData>(input.inputs[0].GetValue<string>());
}

static unique_ptr<GlobalTableFunctionState> TopicsInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<McapMetadataBindData>();
	auto result = make_uniq<VectorGlobalState<TopicRow>>();

	mcap::McapReader reader;
	ThrowIfMcapError(reader.open(bind.path), bind.path);
	ThrowIfMcapError(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan), bind.path);

	auto cache = McapSchemaCache::FromReader(reader);
	auto stats = reader.statistics();
	for (auto &entry : cache.channels) {
		auto count = uint64_t(0);
		if (stats.has_value()) {
			auto count_entry = stats->channelMessageCounts.find(entry.first);
			if (count_entry != stats->channelMessageCounts.end()) {
				count = count_entry->second;
			}
		}
		result->rows.push_back({entry.second.topic, entry.second.schema_name, count,
		                        stats.has_value() ? ToDuckTimestamp(stats->messageStartTime) : timestamp_t(0),
		                        stats.has_value() ? ToDuckTimestamp(stats->messageEndTime) : timestamp_t(0)});
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ChannelsInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<McapMetadataBindData>();
	auto result = make_uniq<VectorGlobalState<ChannelRow>>();

	mcap::McapReader reader;
	ThrowIfMcapError(reader.open(bind.path), bind.path);
	ThrowIfMcapError(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan), bind.path);

	for (auto &entry : reader.channels()) {
		auto channel = entry.second;
		result->rows.push_back({entry.first, channel->topic, channel->schemaId, channel->messageEncoding});
	}
	return std::move(result);
}

static void TopicsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<VectorGlobalState<TopicRow>>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.rows.size()) {
		auto &row = state.rows[state.offset++];
		output.SetValue(0, count, Value(row.topic));
		output.SetValue(1, count, Value(row.type));
		output.SetValue(2, count, Value::UBIGINT(row.count));
		output.SetValue(3, count, Value::TIMESTAMP(row.start));
		output.SetValue(4, count, Value::TIMESTAMP(row.end));
		count++;
	}
	output.SetCardinality(count);
}

static void ChannelsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<VectorGlobalState<ChannelRow>>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.rows.size()) {
		auto &row = state.rows[state.offset++];
		output.SetValue(0, count, Value::USMALLINT(row.id));
		output.SetValue(1, count, Value(row.topic));
		output.SetValue(2, count, Value::USMALLINT(row.schema_id));
		output.SetValue(3, count, Value(row.message_encoding));
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

TableFunction GetMcapTopicsFunction() {
	return TableFunction("mcap_topics", {LogicalTypeId::VARCHAR}, TopicsScan, PathBind, TopicsInitGlobal);
}

TableFunction GetMcapChannelsFunction() {
	return TableFunction("mcap_channels", {LogicalTypeId::VARCHAR}, ChannelsScan, ChannelsBind, ChannelsInitGlobal);
}

} // namespace duckdb
