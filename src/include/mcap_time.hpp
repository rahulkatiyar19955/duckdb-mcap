#pragma once

#include "duckdb/common/types/timestamp.hpp"

#include <mcap/types.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace duckdb {

//! MCAP timestamps are uint64 nanoseconds; DuckDB TIMESTAMP_NS is int64
//! nanoseconds. Clamp instead of wrapping for values beyond int64 range
//! (e.g. mcap::MaxTime sentinels in statistics of empty files).
inline timestamp_ns_t ToTimestampNs(mcap::Timestamp nanos) {
	auto clamped = std::min<uint64_t>(nanos, uint64_t(std::numeric_limits<int64_t>::max()));
	return timestamp_ns_t(static_cast<int64_t>(clamped));
}

} // namespace duckdb
