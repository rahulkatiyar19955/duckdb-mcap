#include "mcap_scan.hpp"

#include "decoder.hpp"
#include "duckdb/common/enums/file_glob_options.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "mcap_file.hpp"
#include "mcap_time.hpp"
#include "protobuf_decoder.hpp"
#include "pushdown.hpp"
#include "ros2_decoder.hpp"
#include "schema_cache.hpp"

#include <mcap/reader.hpp>

#include <algorithm>
#include <atomic>
#include <optional>

namespace duckdb {

namespace {

//! Group roughly this many chunks into one scan partition. Small enough to keep
//! many threads busy on big files, large enough to amortize per-partition setup.
constexpr idx_t CHUNKS_PER_PARTITION = 4;

struct McapScanBindData : public TableFunctionData {
	explicit McapScanBindData(vector<string> paths_p) : paths(std::move(paths_p)) {
	}

	//! Expanded file list (glob patterns resolved at bind time).
	vector<string> paths;
	//! First file's statistics message count scaled by the file count — an
	//! estimate, not a total: binding deliberately opens only one file (globs can
	//! match many, possibly remote files). Unset when the first file lacks stats.
	std::optional<idx_t> estimated_rows;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<McapScanBindData>(paths);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<McapScanBindData>();
		return paths == other.paths;
	}
};

//! One unit of parallel work: a [start, end) log-time window of one file.
//! Windows per file are disjoint and cover the pushdown range, and MCAP's read
//! window is start-inclusive / end-exclusive, so every message belongs to
//! exactly one partition — chunk overlap at boundaries costs only duplicate
//! decompression, never duplicate (or dropped) rows.
struct McapScanPartition {
	idx_t file_index;
	mcap::Timestamp start;
	mcap::Timestamp end;
};

struct McapScanGlobalState : public GlobalTableFunctionState {
	vector<string> paths;
	vector<column_t> column_ids;
	McapPushdown pushdown;
	vector<McapScanPartition> partitions;
	//! Per-file channel/schema registry, built once and read-only afterwards.
	vector<McapSchemaCache> caches;
	std::atomic<idx_t> next_partition {0};
	std::atomic<idx_t> finished_partitions {0};

	idx_t MaxThreads() const override {
		return MaxValue<idx_t>(partitions.size(), 1);
	}
};

struct McapScanLocalState : public LocalTableFunctionState {
	idx_t file_index = DConstants::INVALID_INDEX;
	string current_filename;
	unique_ptr<McapFile> file;
	unique_ptr<mcap::LinearMessageView> view;
	std::optional<mcap::LinearMessageView::Iterator> iterator;
	std::optional<mcap::LinearMessageView::Iterator> end;
	const McapSchemaCache *cache = nullptr;
	//! Decoders cache parsed schemas by per-file schema id, so they are recreated
	//! whenever this thread moves to a different file.
	unique_ptr<ProtobufDecoder> protobuf;
	unique_ptr<Ros2Decoder> ros2;

	//! Claim the next partition; (re)open the file when it differs from the one
	//! this thread already holds. Returns false when no partitions remain.
	bool NextPartition(ClientContext &context, McapScanGlobalState &gstate) {
		auto partition_index = gstate.next_partition.fetch_add(1);
		if (partition_index >= gstate.partitions.size()) {
			return false;
		}
		auto &partition = gstate.partitions[partition_index];
		if (!file || file_index != partition.file_index) {
			file = OpenMcapFile(context, gstate.paths[partition.file_index]);
			file_index = partition.file_index;
			current_filename = gstate.paths[partition.file_index];
			cache = &gstate.caches[partition.file_index];
			protobuf = make_uniq<ProtobufDecoder>();
			ros2 = make_uniq<Ros2Decoder>();
		}
		auto options = ToReadMessageOptions(gstate.pushdown);
		options.startTime = partition.start;
		options.endTime = partition.end;
		auto on_problem = [](const mcap::Status &status) {
			if (!status.ok()) {
				throw IOException("MCAP scan failed: %s", status.message);
			}
		};
		// The old iterators point into the old view: drop them before replacing it.
		iterator.reset();
		end.reset();
		view = make_uniq<mcap::LinearMessageView>(file->reader.readMessages(on_problem, options));
		iterator.emplace(view->begin());
		end.emplace(view->end());
		return true;
	}
};

static unique_ptr<FunctionData> McapScanBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("mcap_scan(path) requires a single non-null path argument");
	}

	auto pattern = input.inputs[0].GetValue<string>();
	auto &fs = FileSystem::GetFileSystem(context);
	vector<string> paths;
	for (auto &file : fs.GlobFiles(pattern, context, FileGlobOptions::DISALLOW_EMPTY)) {
		paths.push_back(file.path);
	}
	std::sort(paths.begin(), paths.end());

	// The scan's schema is fixed, so binding does not need file contents. Open
	// only the first file: that validates the glob matched real MCAP and seeds
	// the optimizer's cardinality estimate, without paying one summary read per
	// matched file (expensive for wide globs and remote filesystems).
	idx_t first_file_rows = 0;
	bool have_statistics = false;
	{
		auto file = OpenMcapFile(context, paths[0]);
		auto &stats = file->reader.statistics();
		if (stats.has_value()) {
			first_file_rows = stats->messageCount;
			have_statistics = true;
		}
	}

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
	names.emplace_back("filename");
	return_types.emplace_back(LogicalTypeId::VARCHAR);

	auto result = make_uniq<McapScanBindData>(std::move(paths));
	if (have_statistics) {
		result->estimated_rows = first_file_rows * result->paths.size();
	}
	// NB: explicit move — GCC does not implicitly convert unique_ptr<Derived>
	// lvalues to unique_ptr<Base> on return.
	return std::move(result);
}

//! Split one file's pushdown window into partitions whose boundaries fall on
//! chunk start times (so partitions align with chunks for I/O efficiency).
//! Files without chunk indexes get a single whole-window partition.
//! NOTE: with a narrowed time window, messages stored *outside* chunks in a
//! file that has chunk indexes can be skipped by mcap's index-based byte-range
//! computation — a pre-existing property of indexed reads (standard writers
//! always chunk their messages).
static void BuildPartitions(idx_t file_index, const mcap::McapReader &reader, const McapPushdown &pushdown,
                            vector<McapScanPartition> &partitions) {
	auto lo = pushdown.start_time;
	auto hi = pushdown.end_time;
	if (lo >= hi) {
		return; // degenerate window: no rows can match
	}
	vector<mcap::Timestamp> bounds;
	bounds.push_back(lo);
	auto &chunks = reader.chunkIndexes();
	if (chunks.size() > CHUNKS_PER_PARTITION) {
		vector<mcap::Timestamp> starts;
		for (auto &chunk : chunks) {
			if (chunk.messageStartTime > lo && chunk.messageStartTime < hi) {
				starts.push_back(chunk.messageStartTime);
			}
		}
		std::sort(starts.begin(), starts.end());
		starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
		for (idx_t i = CHUNKS_PER_PARTITION; i < starts.size(); i += CHUNKS_PER_PARTITION) {
			bounds.push_back(starts[i]);
		}
	}
	bounds.push_back(hi);
	for (idx_t i = 0; i + 1 < bounds.size(); i++) {
		partitions.push_back({file_index, bounds[i], bounds[i + 1]});
	}
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

	result->paths = bind.paths;
	result->pushdown = ExtractMcapPushdown(input);

	// Open each file once: the summary populates the channel/schema registry
	// (needed for schema_name + JSON decoding) and the chunk index that
	// partitioning and per-thread pruning are built from.
	for (idx_t file_index = 0; file_index < result->paths.size(); file_index++) {
		auto file = OpenMcapFile(context, result->paths[file_index]);
		result->caches.push_back(McapSchemaCache::FromReader(file->reader));
		BuildPartitions(file_index, file->reader, result->pushdown, result->partitions);
	}

	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> McapScanInitLocal(ExecutionContext &context, TableFunctionInitInput &,
                                                             GlobalTableFunctionState *gstate_p) {
	auto result = make_uniq<McapScanLocalState>();
	auto &gstate = gstate_p->Cast<McapScanGlobalState>();
	result->NextPartition(context.client, gstate);
	return std::move(result);
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
                                 const std::optional<std::string> &json_payload, const string &filename) {
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
	case McapScanColumn::FILENAME:
		FlatVector::GetData<string_t>(vector)[row] = StringVector::AddString(vector, filename);
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

static void McapScanFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<McapScanGlobalState>();
	auto &state = data.local_state->Cast<McapScanLocalState>();
	idx_t count = 0;
	auto decode_json = NeedsJson(gstate);

	while (count < STANDARD_VECTOR_SIZE) {
		if (!state.iterator.has_value()) {
			break; // this thread never got a partition
		}
		if (*state.iterator == *state.end) {
			gstate.finished_partitions.fetch_add(1);
			if (!state.NextPartition(context, gstate)) {
				// Clear the exhausted iterator so a later call on this local state
				// does not count the same partition as finished again.
				state.iterator.reset();
				state.end.reset();
				break; // every partition consumed
			}
			continue;
		}
		// NOTE: message_view (and message.data) is only valid until the iterator is
		// advanced, so read everything we need before the ++ at the end of the loop.
		const auto &message_view = **state.iterator;

		const auto *channel_info = state.cache->Lookup(message_view.message.channelId);
		std::optional<std::string> json_payload;
		if (decode_json && channel_info) {
			json_payload =
			    DecodePayloadJson(*channel_info, message_view.message, state.protobuf.get(), state.ros2.get());
		}

		for (idx_t output_col = 0; output_col < gstate.column_ids.size(); output_col++) {
			WriteProjectedColumn(output, output_col, count, gstate.column_ids[output_col], message_view, channel_info,
			                     json_payload, state.current_filename);
		}
		count++;
		++(*state.iterator);
	}

	output.SetCardinality(count);
}

static unique_ptr<NodeStatistics> McapScanCardinality(ClientContext &, const FunctionData *bind_data) {
	auto &bind = bind_data->Cast<McapScanBindData>();
	if (!bind.estimated_rows.has_value()) {
		return nullptr;
	}
	return make_uniq<NodeStatistics>(*bind.estimated_rows, *bind.estimated_rows);
}

static double McapScanProgress(ClientContext &, const FunctionData *, const GlobalTableFunctionState *gstate_p) {
	auto &gstate = gstate_p->Cast<McapScanGlobalState>();
	if (gstate.partitions.empty()) {
		return 100.0;
	}
	return 100.0 * double(gstate.finished_partitions.load()) / double(gstate.partitions.size());
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
	function.cardinality = McapScanCardinality;
	function.table_scan_progress = McapScanProgress;
	return function;
}

} // namespace duckdb
