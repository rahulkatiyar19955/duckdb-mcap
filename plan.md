# DuckDB-MCAP Extension: Implementation Plan

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                         SQL                             │
└─────────────────────────┬───────────────────────────────┘
                          ▼
┌─────────────────────────────────────────────────────────┐
│                    DuckDB Planner                        │
│              (Pushdown & Projection Logic)               │
└─────────────────────────┬───────────────────────────────┘
                          ▼
┌─────────────────────────────────────────────────────────┐
│                    MCAP Extension                        │
│              (Table Functions + Custom Ops)                │
└─────────────────────────┬───────────────────────────────┘
                          │
          ┌───────────────┴───────────────┐
          ▼                               ▼
┌─────────────────────┐       ┌─────────────────────┐
│   Metadata Layer    │       │    Scan Engine      │
│                     │       │                     │
│  • Schema Cache     │       │  • Chunk Reader     │
│  • Channel Registry │       │  • Message Decoder  │
│                     │       │                     │
└─────────┬───────────┘       └─────────┬───────────┘
          │                             │
          └───────────────┬─────────────┘
                          ▼
┌─────────────────────────────────────────────────────────┐
│                     MCAP File                            │
│         (Source of Truth — Zero Copy Access)             │
└─────────────────────────────────────────────────────────┘
```

---

## Design Principles

### 1. Zero Copy
**Never do:**
```
MCAP → Convert → DuckDB
```

**Always do:**
```
MCAP → Virtual Tables
```

MCAP remains the single source of truth. No intermediate conversion.

### 2. Pushdown First
Design every component assuming the following query should never touch irrelevant topics:

```sql
SELECT *
FROM mcap_scan('run.mcap')
WHERE topic = '/rosout';
```

This query must **not** read:
- `/camera`
- `/scan`
- `/odom`

### 3. Chunk-Based Execution
MCAP already contains optimized indexes:
- `ChunkIndex`
- `MessageIndex`
- `ChannelInfo`
- `SchemaInfo`

The planner must exploit these aggressively to minimize I/O.

---

## Implementation Phases

### Phase 0 — Technical Discovery
**Duration:** 2–3 days  
**Goal:** Understand both codebases deeply.

#### DuckDB Study
Focus on:
- `TableFunction`, `TableFunctionRef`
- `TableFunctionBindInput`, `TableFunctionData`
- `GlobalTableFunctionState`, `LocalTableFunctionState`
- Pushdown mechanisms (filter & projection)
- `ProjectionPushdown`

Reference implementations:
- `parquet` extension *(primary reference)*
- `json` extension
- `iceberg` extension

#### MCAP Study
Study:
- `mcap::McapReader`
- `mcap::LinearMessageView`
- `mcap::ChunkIndex`
- `mcap::Schema`
- `mcap::Channel`

**Goal:** Know exactly what metadata can be queried without reading message payloads.

---

### Phase 1 — Skeleton Extension
**Goal:** Basic scan works.

```sql
SELECT *
FROM mcap_scan('run.mcap')
LIMIT 10;
```

**Output schema:**
| Column | Type | Description |
|--------|------|-------------|
| `timestamp` | `TIMESTAMP` | Message timestamp |
| `topic` | `VARCHAR` | Topic name |
| `schema` | `VARCHAR` | Schema name |
| `payload_blob` | `BLOB` | Raw bytes (no decoding) |

**Architecture hooks:**
- `Bind()`
- `InitGlobal()`
- `InitLocal()`
- `Scan()`

**Proves:**
- Extension loads correctly
- Table function interface works
- MCAP file reading works

---

### Phase 2 — Metadata Functions
**Goal:** Instant metadata queries with zero message scanning.

```sql
SELECT * FROM mcap_topics('run.mcap');
SELECT * FROM mcap_schemas('run.mcap');
SELECT * FROM mcap_channels('run.mcap');
```

**Example output (`mcap_topics`):**
| Column | Type | Description |
|--------|------|-------------|
| `topic` | `VARCHAR` | Topic name |
| `type` | `VARCHAR` | Message type |
| `count` | `BIGINT` | Message count |
| `start` | `TIMESTAMP` | First message time |
| `end` | `TIMESTAMP` | Last message time |

---

### Phase 3 — Decoded Message Scan
**Goal:** Return decoded payloads as JSON.

```sql
SELECT *
FROM mcap_scan('run.mcap');
```

**Output schema:**
| Column | Type | Description |
|--------|------|-------------|
| `timestamp` | `TIMESTAMP` | Message timestamp |
| `topic` | `VARCHAR` | Topic name |
| `schema_name` | `VARCHAR` | Schema identifier |
| `payload_json` | `JSON` | Decoded message payload |

**Example payload:**
```json
{
  "severity": "ERROR",
  "message": "Planner aborted"
}
```

**Decision:** Use JSON output. Do not flatten ROS messages yet — flattening is a massive rabbit hole. JSON provides immediate usability.

---

### Phase 4 — Predicate Pushdown (Topic Filtering)
**Priority:** Most important phase.

**Goal:** DuckDB sends `WHERE topic = '/rosout'` to the extension. The extension reads only the matching channel, not the entire bag.

**Mechanism:** Implement `TableFilterSet` support.

**Impact:**
- Without pushdown: 10 GB bag = full scan
- With pushdown: 10 GB bag → 10 MB scan for targeted queries

---

### Phase 5 — Timestamp Pushdown (Chunk Pruning)
**Goal:** `WHERE timestamp BETWEEN ...` reads only matching chunks.

**Mechanism:** Use `ChunkIndex.messageStartTime` and `ChunkIndex.messageEndTime` to prune chunks before reading.

**Impact:**
- Without pruning: 10 GB bag is unusable for time-range queries
- With pruning: Sub-second scans become possible

---

### Phase 6 — Projection Pushdown (Topic-Level)
**Goal:** Support column selection to avoid materializing unused data.

Instead of:
```sql
SELECT * FROM mcap_scan('run.mcap');
```

Support:
```sql
SELECT timestamp, topic FROM mcap_scan('run.mcap');
```

Only materialize requested columns. Huge memory savings.

---

### Phase 7 — ROS-Aware Views
**Goal:** Provide specialized views for common ROS topics.

**Example — `rosout`:**
```sql
SELECT *
FROM rosout('run.mcap');
```

Returns structured columns instead of JSON:
| timestamp | severity | node | message |
|-----------|----------|------|---------|

**Planned views:**
- `rosout(...)` — ROS log messages
- `diagnostics(...)` — Diagnostic arrays
- `odom(...)` — Odometry data
- `tf(...)` — Transform data

These are generated views over the base scan.

---

### Phase 8 — Nested Field Access
**Ultimate goal.** Flattened schema access.

```sql
-- Option A: Nested path access
SELECT payload.pose.position.x
FROM odom('run.mcap');

-- Option B: Flattened alias
SELECT position_x
FROM odom('run.mcap');
```

**Complexity:** Requires full schema flattening. Likely months of work. Defer until after v1.0.

---

### Phase 9 — DataPilot Integration
**Goal:** Expose a single MCP tool for agent interaction.

**Tool:**
- `execute_sql(query)`

**Interaction model:**
```
Question → SQL → Observe → SQL → Observe → Answer
```

Everything else disappears behind the SQL interface. The agent operates exactly like a database agent.

---

## Recommended Repository Structure

```
duckdb-mcap/
├── extension/
│   ├── mcap_extension.cpp      # Entry point & registration
│   ├── mcap_scan.cpp           # Core scan logic (Phases 1, 3)
│   ├── mcap_topics.cpp         # Metadata functions (Phase 2)
│   ├── mcap_schemas.cpp        # Schema metadata (Phase 2)
│   ├── pushdown.cpp            # Filter & projection pushdown (Phases 4, 5, 6)
│   ├── decoder.cpp             # Message decoding (Phase 3)
│   └── schema_cache.cpp        # Schema & channel registry
├── tests/
│   ├── scan.test               # Basic scan validation
│   ├── pushdown.test           # Filter pushdown tests
│   ├── pruning.test            # Timestamp/chunk pruning tests
│   └── rosout.test             # ROS-aware view tests
├── benchmark/
│   ├── 1gb.test
│   ├── 10gb.test
│   └── 100gb.test
└── sample_mcaps/               # Test fixtures
```

---

## Success Criteria

| Version | Deliverable | Target |
|---------|-------------|--------|
| **v0.1** | `SELECT * FROM mcap_scan(...) LIMIT 100;` works | Basic scan |
| **v0.2** | `WHERE topic = '/rosout'` pushdown works | Filter pushdown |
| **v0.3** | `WHERE timestamp BETWEEN ...` chunk pruning works | Time-range pruning |
| **v1.0** | 10 GB MCAP, filtered by topic + time range, returns in **<< 1 second** | Production-ready scan |

**Target query for v1.0:**
```sql
SELECT *
FROM mcap_scan('run.mcap')
WHERE topic = '/rosout'
  AND timestamp BETWEEN '2024-01-01' AND '2024-01-02';
```
