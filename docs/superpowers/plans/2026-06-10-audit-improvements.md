# duckdb-mcap Audit Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement all approved audit improvements: correctness fixes (rosout CDR, per-topic ranges, JSON unescape, CDR guards), packaging fixes, `mcap_scan` schema upgrade to TIMESTAMP_NS + new columns, new metadata functions, DuckDB FileSystem/glob/replacement-scan integration, parallel chunk-partitioned scanning, and test/CI hardening.

**Architecture:** The extension keeps its existing layout (`src/*.cpp`, one TU per concern). New cross-cutting pieces: `json_util` (dependency-free JSON scanning, unit-testable), `mcap_file` (DuckDB-FileSystem-backed `mcap::IReadable` + open helper), and a partition-based scan in `mcap_scan.cpp` where the unit of work is `(file_index, [start_ns, end_ns))` — time-range partitions are a *correct* disjoint cover because MCAP's `ReadMessageOptions` start is inclusive and end is exclusive, so every message lands in exactly one partition regardless of chunk overlap.

**Tech Stack:** C++17, DuckDB v1.4.4 extension API (pinned submodule), mcap C++ v2.1.3 (header-only, vendored submodule), libprotobuf, Catch2 (vendored in duckdb), sqllogictest, Python `mcap` writer for fixtures.

**Verification loop:** `./run_tests.sh` (incremental build + all `test/sql/*.test` + C++ unit tests). Single test: `./run_tests.sh <name>`. C++ tests only: `./run_cpp_tests.sh`.

**Git:** branch `feat/audit-improvements`, one commit per task, single PR at the end. NO Co-Authored-By trailers (user global rule).

**Verified API facts (do not re-derive):**
- `mcap::ReadMessageOptions`: `startTime` inclusive, `endTime` exclusive; `topicFilter`; `readOrder` defaults FileOrder.
- `mcap::IReadable`: `uint64_t size() const`, `uint64_t read(std::byte **output, uint64_t offset, uint64_t size)` (return 0 on failure; buffer must stay valid until next read()).
- `mcap::RecordReader(IReadable&, ByteOffset start, ByteOffset end = EndOffset)` with `std::optional<Record> next()`, `const Status &status()`; `McapReader::ParseMessageIndex/ParseAttachment/ParseMetadata(const Record&, T*)` are public statics; `McapReader::dataSource()` returns `IReadable*`.
- `McapReader`: `chunkIndexes()`, `attachmentIndexes()` (multimap name→AttachmentIndex), `metadataIndexes()`, `statistics()` (optional, has `messageCount`, `channelMessageCounts`, counts, file-wide `messageStartTime/messageEndTime`), `header()` (optional Header{profile, library}).
- DuckDB: `timestamp_ns_t : timestamp_t` (int64 ns); `Value::TIMESTAMPNS(timestamp_ns_t)`; `TimestampNSValue::Get(const Value&)`; `Value::DefaultTryCastAs(const LogicalType&, Value &out, string *err, bool strict=false) const`; `LogicalTypeId::TIMESTAMP_NS`.
- DuckDB FS: `FileSystem::GetFileSystem(context)`, `OpenFile(path, FileFlags::FILE_FLAGS_READ)`, `FileHandle::Read(void*, idx_t nr_bytes, idx_t location)` (positional), `FileHandle::GetFileSize()`, `fs.GlobFiles(path, context, FileGlobOptions::DISALLOW_EMPTY)` → `vector<OpenFileInfo>` (`.path`).
- Replacement scan: `duckdb/function/replacement_scan.hpp`; register via `DBConfig::GetConfig(loader.GetDatabaseInstance()).replacement_scans.emplace_back(fn)`; helpers `ReplacementScan::CanReplace(name, {"mcap"})`, `ReplacementScan::GetFullPath(input)`.
- `TableFunction::cardinality`: `unique_ptr<NodeStatistics>(ClientContext&, const FunctionData*)`; `NodeStatistics(estimated, max)` from `duckdb/storage/statistics/node_statistics.hpp`. `table_scan_progress`: `double(ClientContext&, const FunctionData*, const GlobalTableFunctionState*)`, return 0–100.
- Python fixture env (venv): mcap==1.3.1, protobuf==7.35.0, lz4==4.4.5. `Writer.add_attachment(create_time, log_time, name, media_type, data)`, `Writer.add_metadata(name, dict)`, `Writer(stream, compression=CompressionType.LZ4)`.
- Fixture epoch: `1_700_000_000_000_000_000` ns = `2023-11-14 22:13:20`.

---

### Task 1: Branch, hygiene, vcpkg.json, CI arm64

**Files:**
- Move: `plan.md` → `docs/plan.md`, `implementation.md` → `docs/implementation.md`
- Modify: `.gitignore`, `vcpkg.json`, `.github/workflows/ci.yml`

- [ ] **Step 1: Create branch**
```bash
git checkout -b feat/audit-improvements
```
- [ ] **Step 2: Move dev notes, fix .gitignore**
```bash
git mv plan.md docs/plan.md && git mv implementation.md docs/implementation.md
```
Append to `.gitignore`: `duckdb_unittest_tempdir/`
- [ ] **Step 3: Fix vcpkg.json** (protobuf was missing; `mcap` is unused — headers come from submodule)
```json
{
  "dependencies": [
    "zstd",
    "lz4",
    "protobuf"
  ]
}
```
- [ ] **Step 4: Add linux_arm64 to ci.yml matrix** (mirrors release.yml)
```yaml
          - platform: linux_arm64
            runner: ubuntu-22.04-arm
```
- [ ] **Step 5: Commit**
```bash
git add -A && git commit -m "chore: repo hygiene, fix vcpkg deps, test linux_arm64 in CI"
```

### Task 2: Extract json_util + fix JSON unescaping (TDD)

**Files:**
- Create: `src/include/json_util.hpp`, `src/json_util.cpp`, `test/cpp/test_json_util.cpp`
- Modify: `src/decoder.cpp` (remove scanner code), `src/include/decoder.hpp` (drop `ExtractJsonStringField` decl), `src/views/rosout.cpp` (include json_util.hpp), `CMakeLists.txt` (add source), `run_cpp_tests.sh` (compile new test TU)

Why extract: `decoder.cpp` links against the protobuf/ros2 decoders, dragging libprotobuf into the unit-test binary. The JSON scanner is dependency-free; its own TU makes it directly testable.

- [ ] **Step 1: Write failing tests** — `test/cpp/test_json_util.cpp` (no `CATCH_CONFIG_MAIN`; test_pushdown.cpp owns main):
```cpp
#include "catch.hpp"
#include "json_util.hpp"
using duckdb::ExtractJsonStringField;

TEST_CASE("extracts top-level string field", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"a":"x","b":"y"})", "b") == "y");
}
TEST_CASE("unescapes standard escapes", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"m":"line1\nline2\t\"q\"\\"})", "m") == "line1\nline2\t\"q\"\\");
}
TEST_CASE("unescapes \\u BMP and surrogate pairs", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"m":"Aé"})", "m") == "A\xc3\xa9");
	REQUIRE(ExtractJsonStringField(R"({"m":"😀"})", "m") == "\xf0\x9f\x98\x80");
}
TEST_CASE("ignores nested keys and string-embedded braces", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"o":{"k":"inner"},"k":"outer"})", "k") == "outer");
	REQUIRE(ExtractJsonStringField(R"({"a":"fake } k","k":"real"})", "k") == "real");
}
TEST_CASE("returns numeric scalar as raw token", "[json_util]") {
	REQUIRE(ExtractJsonStringField(R"({"level":40})", "level") == "40");
}
TEST_CASE("missing key / malformed input", "[json_util]") {
	REQUIRE(!ExtractJsonStringField(R"({"a":"x"})", "b").has_value());
	REQUIRE(!ExtractJsonStringField(R"("not an object")", "a").has_value());
	REQUIRE(!ExtractJsonStringField(R"({"a":"unterminated)", "a").has_value());
}
```
- [ ] **Step 2: Create json_util.hpp/cpp.** Header declares `std::optional<std::string> ExtractJsonStringField(std::string_view json, std::string_view key);`. Move the scanner from decoder.cpp verbatim, then fix `ReadJsonString`'s escape branch to:
```cpp
		if (c == '\\' && pos + 1 < json.size()) {
			char esc = json[pos + 1];
			pos += 2;
			if (esc == 'u') {
				// \uXXXX -> code point (combining surrogate pairs) -> UTF-8
				uint32_t cp;
				if (!ReadHex4(json, pos, cp)) {
					return false;
				}
				if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < json.size() && json[pos] == '\\' &&
				    json[pos + 1] == 'u') {
					size_t save = pos;
					uint32_t low;
					pos += 2;
					if (ReadHex4(json, pos, low) && low >= 0xDC00 && low <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					} else {
						pos = save; // lone high surrogate: emit replacement below
						cp = 0xFFFD;
					}
				} else if (cp >= 0xD800 && cp <= 0xDFFF) {
					cp = 0xFFFD;
				}
				if (out) {
					AppendUtf8(*out, cp);
				}
			} else {
				if (out) {
					switch (esc) {
					case 'n': out->push_back('\n'); break;
					case 't': out->push_back('\t'); break;
					case 'r': out->push_back('\r'); break;
					case 'b': out->push_back('\b'); break;
					case 'f': out->push_back('\f'); break;
					default:  out->push_back(esc);  break; // '"', '\\', '/'
					}
				}
			}
			pos--; // loop's pos++ re-advances
			continue;
		}
```
with file-local helpers:
```cpp
static bool ReadHex4(std::string_view json, size_t &pos, uint32_t &out) {
	if (pos + 4 > json.size()) return false;
	out = 0;
	for (int i = 0; i < 4; i++) {
		char h = json[pos + i];
		uint32_t v;
		if (h >= '0' && h <= '9') v = h - '0';
		else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
		else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
		else return false;
		out = (out << 4) | v;
	}
	pos += 4;
	return true;
}
static void AppendUtf8(std::string &out, uint32_t cp) {
	if (cp < 0x80) { out.push_back(char(cp)); }
	else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
	else if (cp < 0x10000) { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
	else { out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
}
```
NOTE the escape branch above replaces a `continue`-based loop body; adjust the surrounding loop so `pos` bookkeeping stays consistent (the simplest correct restructure: convert the for-loop to `while (pos < json.size())` with explicit `pos++` per branch).
- [ ] **Step 3: Wire up.** decoder.cpp keeps only `DecodePayloadJson`; decoder.hpp drops the extractor decl; rosout.cpp includes `json_util.hpp`; CMake `EXTENSION_SOURCES` adds `src/json_util.cpp`; run_cpp_tests.sh compiles `test/cpp/test_pushdown.cpp test/cpp/test_json_util.cpp src/pushdown.cpp src/json_util.cpp`.
- [ ] **Step 4: Run** `./run_tests.sh` → all SQL + C++ tests pass (new tests fail before Step 2's fix, pass after).
- [ ] **Step 5: Commit** `fix: correct JSON string unescaping; extract json_util for unit testing`

### Task 3: CDR decoder hardening + fixtures

**Files:**
- Modify: `src/ros2_decoder.cpp`, `test/generate_ros2_mcap.py`, `test/sql/ros2.test`

- [ ] **Step 1: Extend the fixture generator** with three new channels on the same schema: `/sample_be` (big-endian XCDR1: pack with `>` and encapsulation `b"\x00\x00\x00\x00"`), `/bad` (first 10 bytes of a valid LE body — truncated mid-message), `/xcdr2` (valid LE body but encapsulation `b"\x00\x07\x00\x00"`). Add a `CdrWriterBE` mirroring `CdrWriter` with big-endian struct formats. Log times: 1_700_000_002/3/4 *10^9.
- [ ] **Step 2: Add failing SQL expectations** to `test/sql/ros2.test`:
```
query TT
SELECT topic, payload_json->>'$.label' FROM mcap_scan('test/mcap/ros2.mcap') WHERE topic='/sample_be';
----
/sample_be	gamma

query TT
SELECT topic, payload_json IS NULL FROM mcap_scan('test/mcap/ros2.mcap') WHERE topic IN ('/bad','/xcdr2') ORDER BY topic;
----
/bad	true
/xcdr2	true
```
(update any existing whole-file counts in ros2.test for the 3 new rows)
- [ ] **Step 3: Implement decoder fixes** in ros2_decoder.cpp:
  - `Decode()` encapsulation guard: `if (reader.data[0] != 0 || reader.data[1] > 1) return std::nullopt;` (XCDR1 plain CDR only — XCDR2 has different alignment rules and would silently misparse), then `little_endian = (data[1] == 1)`.
  - `AppendPrimitive`: move `"char"` from the int8 branch to the uint8 branch (ROS2: both `char` and `byte` are uint8-valued).
  - `AppendPrimitive`: `wstring` returns `false` (UTF-16 CDR encoding is vendor-ambiguous; NULL beats garbage). Remove it from the `string` branch; keep it in `PrimitiveTypes()` so parsing still recognizes it.
- [ ] **Step 4: Regenerate fixture + run** `python test/generate_ros2_mcap.py && ./run_tests.sh` → green.
- [ ] **Step 5: Commit** `fix: CDR decoder rejects non-XCDR1, corrects char/byte sign, bails on wstring`

### Task 4: rosout works on real (CDR) ROS2 bags

**Files:**
- Modify: `src/views/rosout.cpp`, `test/generate_ros2_mcap.py`, `test/sql/rosout.test`

- [ ] **Step 1: Fixture** — add to generate_ros2_mcap.py a `rcl_interfaces/msg/Log` schema (`ros2msg`):
```
builtin_interfaces/Time stamp
uint8 level
string name
string msg
string file
string function
uint32 line

================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
```
`CdrWriter.u8()` helper (no alignment needed for size 1). Two `/rosout` messages: `(level=40, name="planner", msg="Planner aborted (cdr)")`, `(level=20, name="lidar_driver", msg="Scanner online")`.
- [ ] **Step 2: Failing test** in rosout.test:
```
query TTT
SELECT severity, node, message FROM rosout('test/mcap/ros2.mcap') ORDER BY timestamp;
----
ERROR	planner	Planner aborted (cdr)
INFO	lidar_driver	Scanner online
```
- [ ] **Step 3: Implement** in rosout.cpp:
  - Global state gains `Ros2Decoder ros2;` and the decode call becomes `DecodePayloadJson(*channel_info, message_view.message, nullptr, &state.ros2);`
  - Severity: try `"severity"` then `"level"`; map numeric strings: 10→DEBUG, 20→INFO, 30→WARN, 40→ERROR, 50→FATAL (non-numeric or unknown pass through):
```cpp
static std::optional<std::string> MapSeverity(std::optional<std::string> raw) {
	if (!raw.has_value()) return raw;
	if (*raw == "10") return std::string("DEBUG");
	if (*raw == "20") return std::string("INFO");
	if (*raw == "30") return std::string("WARN");
	if (*raw == "40") return std::string("ERROR");
	if (*raw == "50") return std::string("FATAL");
	return raw;
}
```
- [ ] **Step 4: Regenerate + run** → green. **Step 5: Commit** `fix: rosout decodes CDR-encoded /rosout and maps numeric severity`

### Task 5: mcap_topics exact per-topic start/end

**Files:**
- Modify: `src/mcap_topics.cpp`, `test/sql/metadata.test`

- [ ] **Step 1: Failing test** (sample.mcap: /rosout at 22:13:20, /odom at 22:15:00):
```
query TTT
SELECT topic, start, "end" FROM mcap_topics('test/mcap/sample.mcap') ORDER BY topic;
----
/odom	2023-11-14 22:15:00	2023-11-14 22:15:00
/rosout	2023-11-14 22:13:20	2023-11-14 22:13:20
```
- [ ] **Step 2: Implement.** In `TopicsInitGlobal`, compute per-channel ranges from message indexes:
```cpp
std::unordered_map<mcap::ChannelId, std::pair<mcap::Timestamp, mcap::Timestamp>> ranges;
auto *source = reader.dataSource();
for (auto &chunk_index : reader.chunkIndexes()) {
	for (auto &offset_entry : chunk_index.messageIndexOffsets) {
		mcap::RecordReader record_reader(*source, offset_entry.second);
		auto record = record_reader.next();
		if (!record.has_value() || !record_reader.status().ok()) continue;
		mcap::MessageIndex message_index;
		if (!mcap::McapReader::ParseMessageIndex(*record, &message_index).ok()) continue;
		for (auto &rec : message_index.records) {
			auto it = ranges.find(message_index.channelId);
			if (it == ranges.end()) ranges.emplace(message_index.channelId, std::make_pair(rec.first, rec.first));
			else { it->second.first = std::min(it->second.first, rec.first); it->second.second = std::max(it->second.second, rec.first); }
		}
	}
}
```
`TopicRow.start/end` become `Value` (NULL when channel has no message-index data); `TopicsScan` uses `output.SetValue` with those Values directly.
- [ ] **Step 3: Run + commit** `fix: mcap_topics reports exact per-topic time ranges from message indexes`

### Task 6: TIMESTAMP_NS + publish_time/sequence/channel_id columns

**Files:**
- Create: `src/include/mcap_time.hpp`
- Modify: `src/include/mcap_scan.hpp`, `src/mcap_scan.cpp`, `src/pushdown.cpp`, `src/views/rosout.cpp`, `src/mcap_topics.cpp`, `test/cpp/test_pushdown.cpp`, any `test/sql/*.test` with schema/timestamp expectations

- [ ] **Step 1: Shared time helper** `src/include/mcap_time.hpp`:
```cpp
#pragma once
#include "duckdb/common/types/timestamp.hpp"
#include <mcap/types.hpp>
#include <algorithm>
#include <limits>
namespace duckdb {
inline timestamp_ns_t ToTimestampNs(mcap::Timestamp nanos) {
	auto clamped = std::min<uint64_t>(nanos, uint64_t(std::numeric_limits<int64_t>::max()));
	return timestamp_ns_t(static_cast<int64_t>(clamped));
}
} // namespace duckdb
```
- [ ] **Step 2: Schema change.** `McapScanColumn` adds `PUBLISH_TIME = 5, SEQUENCE = 6, CHANNEL_ID = 7` (FILENAME comes in Task 10); `MCAP_SCAN_COLUMN_COUNT = 8`. Bind: `timestamp` → `LogicalType::TIMESTAMP_NS`, append `publish_time TIMESTAMP_NS`, `sequence UINTEGER`, `channel_id USMALLINT`. `WriteProjectedColumn` cases write `ToTimestampNs(message.logTime/publishTime)` via `FlatVector::GetData<timestamp_t>`, `message.sequence` (uint32), `message.channelId` (uint16). rosout + mcap_topics timestamps also become TIMESTAMP_NS via the helper (drop the three `ToDuckTimestamp` copies).
- [ ] **Step 3: Pushdown rewrite** in pushdown.cpp — value conversion handles TIMESTAMP_NS natively, widening shrinks from 1000ns to 1ns:
```cpp
static bool TimestampFilterValueToNanos(const Value &value, mcap::Timestamp &out) {
	if (value.IsNull()) return false;
	int64_t ns;
	if (value.type().id() == LogicalTypeId::TIMESTAMP_NS) {
		ns = TimestampNSValue::Get(value).value;
	} else {
		Value cast;
		string error;
		if (!value.DefaultTryCastAs(LogicalType::TIMESTAMP_NS, cast, &error) || cast.IsNull()) {
			return false; // unbounded: caller must not restrict
		}
		ns = TimestampNSValue::Get(cast).value;
	}
	out = ns < 0 ? 0 : static_cast<mcap::Timestamp>(ns);
	return true;
}
```
`CollectTimeRange` CONSTANT_COMPARISON: on conversion failure return false; `EQUAL → [ns, Sat(ns,1))`, `> → [Sat(ns,1), Max)`, `>= → [ns, Max)`, `< → [0, ns)`, `<= → [0, Sat(ns,1))`. Includes: `duckdb/common/types/timestamp.hpp`, `duckdb/common/types/value.hpp`.
- [ ] **Step 4: Update C++ tests.** `TsCompare` builds `Value::TIMESTAMPNS(timestamp_ns_t(nanos))`; the `NS` factor stays only for readable literals; every expected `+1000`/`SatAdd(...,1000)` widening becomes `+1`. Re-derive each TEST_CASE expectation accordingly (semantics: exclusive end = value+1 for `=`/`<=`).
- [ ] **Step 5: Update SQL tests.** Whole-second fixture timestamps render identically under TIMESTAMP_NS; fix any test asserting column types/counts or `SELECT *` width. Run full suite, fix mechanical expectations only.
- [ ] **Step 6: Commit** `feat: nanosecond timestamps + publish_time/sequence/channel_id columns in mcap_scan`

### Task 7: Protobuf decoder caching

**Files:**
- Modify: `src/protobuf_decoder.cpp`

- [ ] **Step 1:** `Impl` gains `std::unordered_map<mcap::SchemaId, gp::Message *> instances;` (values owned by `std::vector<std::unique_ptr<gp::Message>> owned;`). `Decode()` becomes: look up instance by `channel.schema_id`; on miss do the existing LoadSchema + FindMessageTypeByName + `factory.GetPrototype(descriptor)->New()` once and cache (cache `nullptr` for unresolvable schemas to avoid re-trying per row); per row: `instance->Clear(); instance->ParseFromArray(...)`.
- [ ] **Step 2:** `./run_tests.sh protobuf` then full suite → green. Commit `perf: cache protobuf message instances per schema`

### Task 8: mcap_info / mcap_attachments / mcap_metadata

**Files:**
- Create: `src/mcap_records.cpp`, `test/sql/records.test`
- Modify: `src/include/mcap_metadata.hpp` (3 new Get*Function decls), `src/mcap_extension.cpp` (register), `CMakeLists.txt`, `test/generate_sample_mcap.py`

- [ ] **Step 1: Fixture** — sample generator adds:
```python
writer.add_attachment(create_time=1_700_000_000_000_000_000, log_time=1_700_000_000_000_000_000,
                      name="notes.txt", media_type="text/plain", data=b"hello attachment")
writer.add_metadata("session", {"vehicle": "amr-7", "site": "plant-3"})
```
- [ ] **Step 2: Failing tests** `test/sql/records.test`:
```
query TTII
SELECT profile, library IS NOT NULL, message_count, chunk_count > 0 FROM mcap_info('test/mcap/sample.mcap');
----
json	true	2	true

query TTIT
SELECT name, media_type, data_size, data FROM mcap_attachments('test/mcap/sample.mcap');
----
notes.txt	text/plain	16	hello attachment

query TTT
SELECT name, key, value FROM mcap_metadata('test/mcap/sample.mcap') ORDER BY key;
----
session	site	plant-3
session	vehicle	amr-7
```
- [ ] **Step 3: Implement** `src/mcap_records.cpp` — three table functions following the existing `VectorGlobalState<Row>` pattern from mcap_topics.cpp:
  - `mcap_info(path)`: one row; columns `profile VARCHAR, library VARCHAR, message_count UBIGINT, channel_count UINTEGER, schema_count UINTEGER, chunk_count UINTEGER, attachment_count UINTEGER, metadata_count UINTEGER, start TIMESTAMP_NS, "end" TIMESTAMP_NS` from `reader.header()` + `reader.statistics()` (NULLs where absent).
  - `mcap_attachments(path)`: iterate `reader.attachmentIndexes()`; per entry `mcap::RecordReader rr(*reader.dataSource(), idx.offset); auto rec = rr.next();` → `McapReader::ParseAttachment` → row `{name, media_type, log_time TIMESTAMP_NS, create_time TIMESTAMP_NS, data_size UBIGINT, data BLOB}` (copy `attachment.data, attachment.dataSize` into a string at init — the record buffer dies with the reader).
  - `mcap_metadata(path)`: iterate `reader.metadataIndexes()`; `ParseMetadata`; one row per kv: `{name, key, value}`.
- [ ] **Step 4:** Register all three in `LoadInternal`; add source to CMake; regenerate fixture; run; commit `feat: mcap_info, mcap_attachments, mcap_metadata table functions`

### Task 9: DuckDB FileSystem-backed IReadable

**Files:**
- Create: `src/include/mcap_file.hpp`, `src/mcap_file.cpp`
- Modify: all `Init*Global` call sites (`mcap_scan.cpp`, `mcap_topics.cpp`, `mcap_schemas.cpp`, `mcap_records.cpp`, `views/rosout.cpp`), `CMakeLists.txt`

- [ ] **Step 1: Implement**
```cpp
// mcap_file.hpp
#pragma once
#include "duckdb/common/file_system.hpp"
#include <mcap/reader.hpp>
namespace duckdb {
//! mcap::IReadable over a DuckDB FileHandle, enabling any registered DuckDB
//! filesystem (local, httpfs/S3, ...) as an MCAP source. Not thread-safe; one
//! instance per reader.
class DuckDBFileReadable final : public mcap::IReadable {
public:
	DuckDBFileReadable(FileSystem &fs, const std::string &path)
	    : handle(fs.OpenFile(path, FileFlags::FILE_FLAGS_READ)),
	      file_size(static_cast<uint64_t>(handle->GetFileSize())) {}
	uint64_t size() const override { return file_size; }
	uint64_t read(std::byte **output, uint64_t offset, uint64_t size) override;
private:
	unique_ptr<FileHandle> handle;
	uint64_t file_size;
	std::vector<std::byte> buffer;
};
//! An opened MCAP file: keeps the readable alive for the reader's lifetime.
struct McapFile {
	unique_ptr<DuckDBFileReadable> readable; // declared before reader: destroyed after it
	mcap::McapReader reader;
	~McapFile() { reader.close(); }
};
//! Open + readSummary via DuckDB's FileSystem; throws IOException on failure.
unique_ptr<McapFile> OpenMcapFile(ClientContext &context, const std::string &path);
} // namespace duckdb
```
```cpp
// mcap_file.cpp read():
uint64_t DuckDBFileReadable::read(std::byte **output, uint64_t offset, uint64_t size) {
	if (offset >= file_size) return 0;
	auto to_read = std::min<uint64_t>(size, file_size - offset);
	try {
		buffer.resize(to_read);
		handle->Read(buffer.data(), to_read, offset);
	} catch (...) {
		return 0; // mcap contract: 0 signals read failure
	}
	*output = buffer.data();
	return to_read;
}
```
`OpenMcapFile` = construct readable (catch IOException from OpenFile and rethrow with mcap context), `reader.open(*readable)`, `reader.readSummary(AllowFallbackScan)`, throwing `IOException` on bad Status (reuse existing message format).
- [ ] **Step 2: Convert all five init sites** to `auto file = OpenMcapFile(context, path);` (Init functions get `ClientContext &context` from their signatures — un-anonymize the param). Global states store `unique_ptr<McapFile>`.
- [ ] **Step 3:** Full suite green (local FS path now exercised through DuckDB FS). Commit `feat: read MCAP through DuckDB FileSystem (enables httpfs/S3)`

### Task 10: Glob/multi-file + filename column + replacement scan

**Files:**
- Modify: `src/mcap_scan.cpp` (+`mcap_scan.hpp`), `src/mcap_extension.cpp`, `test/sql/scan.test`

- [ ] **Step 1: Failing tests** (scan.test):
```
query I
SELECT count(*) FROM mcap_scan('test/mcap/*.mcap');
----
<sum of fixture rows at this point>

query I
SELECT count(DISTINCT filename) FROM mcap_scan('test/mcap/*.mcap');
----
3

query I
SELECT count(*) FROM 'test/mcap/sample.mcap';
----
2
```
- [ ] **Step 2: Bind globs.** `McapScanBindData.paths` (vector<string>) via `FileSystem::GetFileSystem(context).GlobFiles(path, context, FileGlobOptions::DISALLOW_EMPTY)`. Add `FILENAME = 8` to `McapScanColumn` (`COUNT = 9`), bind appends `filename VARCHAR`. Scan writes `paths[file_index]`.
- [ ] **Step 3: Multi-file iteration.** Global state: `idx_t current_file` + per-file open/init refactored into a helper; when the iterator hits end, advance to next file (this becomes partition-based in Task 11 — keep the refactor minimal: extract `InitFile(idx)`).
- [ ] **Step 4: Replacement scan** in mcap_extension.cpp:
```cpp
static unique_ptr<TableRef> McapReplacementScan(ClientContext &, ReplacementScanInput &input,
                                                optional_ptr<ReplacementScanData>) {
	auto table_name = ReplacementScan::GetFullPath(input);
	if (!ReplacementScan::CanReplace(table_name, {"mcap"})) {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>("mcap_scan", std::move(children));
	return std::move(table_function);
}
// in LoadInternal:
auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
config.replacement_scans.emplace_back(McapReplacementScan);
```
Includes: `duckdb/function/replacement_scan.hpp`, `duckdb/parser/tableref/table_function_ref.hpp`, `duckdb/parser/expression/function_expression.hpp`, `duckdb/parser/expression/constant_expression.hpp`, `duckdb/main/config.hpp`.
- [ ] **Step 5:** Run, fix counts, commit `feat: glob/multi-file mcap_scan with filename column; FROM 'x.mcap' replacement scan`

### Task 11: Partitioned parallel scan

**Files:**
- Modify: `src/mcap_scan.cpp`, `test/sql/scan.test`

Design (correctness argument): partitions are `(file_index, [start_ns, end_ns))` with ranges forming a disjoint cover of the pushdown window per file. MCAP `startTime` is inclusive / `endTime` exclusive, so each message's logTime falls in exactly one partition — no dupes/drops even when chunks overlap boundaries (a boundary chunk is just decompressed by ≤2 partitions). Boundaries are chunk `messageStartTime`s (deduped, sorted, grouped ~4 chunks/partition) so partitions align with chunk starts for I/O efficiency. Files without chunk indexes → one whole-file partition.

- [ ] **Step 1:** Global state drops reader/view/iterator and gains:
```cpp
struct McapScanPartition { idx_t file_index; mcap::Timestamp start; mcap::Timestamp end; };
vector<McapScanPartition> partitions;
vector<shared_ptr<McapSchemaCache>> caches;   // per file, built once in InitGlobal
std::atomic<idx_t> next_partition {0};
std::atomic<idx_t> finished_partitions {0};
idx_t MaxThreads() const override { return MaxValue<idx_t>(partitions.size(), 1); }
```
InitGlobal: per file `OpenMcapFile` → cache → partition boundaries from `reader.chunkIndexes()` clipped to `[pushdown.start_time, pushdown.end_time)`.
- [ ] **Step 2:** Local state (`McapScanLocalState`) owns `unique_ptr<McapFile> file`, view/iterators, decoders, `idx_t file_index = INVALID`; `bool NextPartition(ClientContext&, global)` claims `next_partition++`, (re)opens the file if `file_index` changed (also resets decoders — schema ids are per-file), builds `LinearMessageView` with options = pushdown ∩ partition range. Scan loop: emit until chunk full; on iterator end, `finished_partitions++` and claim next partition; return 0-cardinality chunk only when no partitions remain. `McapScanInitLocal` gets the real `ExecutionContext &context` (use `context.client`).
- [ ] **Step 3:** Parallel smoke test in scan.test (fixtures are small; this mostly proves no crashes/dupes under the partitioned path — counts already covered above):
```
statement ok
PRAGMA threads=4;

query I
SELECT count(*) FROM mcap_scan('test/mcap/*.mcap');
----
<same total>
```
- [ ] **Step 4:** Full suite; commit `feat: parallel chunk-partitioned mcap_scan`

### Task 12: Cardinality estimate + progress

**Files:**
- Modify: `src/mcap_scan.cpp`

- [ ] **Step 1:** Bind reads each file's summary (`OpenMcapFile`) and sums `statistics()->messageCount` into `optional<idx_t> estimated_rows` (unset if any file lacks statistics).
- [ ] **Step 2:**
```cpp
static unique_ptr<NodeStatistics> McapScanCardinality(ClientContext &, const FunctionData *bind_data) {
	auto &bind = bind_data->Cast<McapScanBindData>();
	if (!bind.estimated_rows.has_value()) return nullptr;
	return make_uniq<NodeStatistics>(*bind.estimated_rows, *bind.estimated_rows);
}
static double McapScanProgress(ClientContext &, const FunctionData *, const GlobalTableFunctionState *gstate) {
	auto &state = gstate->Cast<McapScanGlobalState>();
	if (state.partitions.empty()) return 100.0;
	return 100.0 * double(state.finished_partitions.load()) / double(state.partitions.size());
}
```
wired as `function.cardinality = ...; function.table_scan_progress = ...;` (include `duckdb/storage/statistics/node_statistics.hpp`).
- [ ] **Step 3:** Full suite; commit `feat: cardinality estimate and scan progress for mcap_scan`

### Task 13: lz4 fixture, pinned fixture deps, CI drift check

**Files:**
- Create: `test/requirements.txt`
- Modify: `test/generate_sample_mcap.py`, `test/sql/scan.test`, `.github/workflows/ci.yml`, `run_tests.sh`

- [ ] **Step 1:** `test/requirements.txt`: `mcap==1.3.1`, `protobuf==7.35.0`, `lz4==4.4.5`.
- [ ] **Step 2:** Sample generator also writes `test/mcap/sample_lz4.mcap` — same two messages, `Writer(stream, compression=CompressionType.LZ4)` (refactor body into `write_messages(writer)` reused for both outputs).
- [ ] **Step 3:** scan.test: `SELECT count(*) FROM mcap_scan('test/mcap/sample_lz4.mcap');` → 2, plus a payload equality probe vs sample.mcap. Update glob-count expectations (+1 file). run_tests.sh fixture-presence check includes sample_lz4.mcap.
- [ ] **Step 4:** ci.yml: `pip install -r test/requirements.txt` (replacing ad-hoc installs) and after regeneration: `git diff --exit-code -- test/mcap` (drift check). Same pinned install in release? release.yml doesn't regenerate — leave it.
- [ ] **Step 5:** Regenerate, run, commit `test: lz4 fixture, pinned fixture deps, CI fixture-drift check`

### Task 14: Negative tests, README, final verification, PR

**Files:**
- Create: `test/sql/errors.test`
- Modify: `README.md`

- [ ] **Step 1:** errors.test: nonexistent file → `statement error` (both mcap_scan + mcap_topics); empty-but-valid mcap (add `test/mcap/empty.mcap` to sample generator: start+finish only) → `count(*) = 0` and `mcap_topics` returns 0 rows.
- [ ] **Step 2:** README: new schema (9 columns, TIMESTAMP_NS), new functions (mcap_info/attachments/metadata), glob + `FROM 'x.mcap'` + httpfs note, parallel scan note, severity mapping, requirements.txt bootstrap, updated decoding table (wstring → NULL, XCDR1 only).
- [ ] **Step 3: Full verification** (superpowers:verification-before-completion): `./run_tests.sh` end-to-end green output captured.
- [ ] **Step 4:** Push branch, open PR with summary of all 19 items (no Co-Authored-By):
```bash
git push -u origin feat/audit-improvements
gh pr create --title "Audit improvements: correctness, FS integration, parallel scan, new functions" --body "..."
```

## Self-Review

- **Spec coverage:** items 1(T4), 2(T5), 3(T2), 4(T3), 5(T1), 6(T1), 7(T13), 8(T6), 9(T12), 10(T7), 11+12(T8), 13(T9), 14+15(T10), 16(T11), 18(T13), 19(T3,T14), 20(T1). Windows (17) excluded per approval. ✓
- **Type consistency:** `McapFile`/`OpenMcapFile` used in T9–T12; `McapScanColumn` extended T6 then T10; `estimated_rows` defined T12 where used. ✓
- **Known judgment calls baked in:** time-partition correctness relies on exclusive endTime (verified); messages outside chunks in files *with* chunk indexes may be skipped by mcap's byteRange — pre-existing upstream behavior, standard writers always chunk; documented in code comment (T11).
