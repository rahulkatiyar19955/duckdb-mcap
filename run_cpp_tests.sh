#!/usr/bin/env bash
# Build and run the standalone C++ unit tests for the mcap extension.
#
# These cover the pushdown filter logic (src/pushdown.cpp) directly with Catch2,
# complementing the SQL sqllogictests in test/sql. They link against the libduckdb
# produced by a normal extension build, so run a build first (./run_tests.sh or
# `make release`).
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build/release}"
LIB_DIR="${BUILD_DIR}/src"
CXX="${CXX:-c++}"
OUT="${BUILD_DIR}/mcap_cpp_unittest"

# libduckdb is a .dylib on macOS and a .so on Linux.
if [[ ! -e "${LIB_DIR}/libduckdb.dylib" && ! -e "${LIB_DIR}/libduckdb.so" ]]; then
    echo "!! libduckdb not found in ${LIB_DIR}; build the extension first (e.g. ./run_tests.sh)" >&2
    exit 1
fi

echo ">> compiling C++ unit tests..."
"${CXX}" -std=c++17 -O1 -g \
    -I src/include \
    -I mcap_repo/cpp/mcap/include \
    -I duckdb/src/include \
    -I duckdb/third_party/catch \
    test/cpp/test_pushdown.cpp test/cpp/test_json_util.cpp src/pushdown.cpp src/json_util.cpp \
    -L "${LIB_DIR}" -lduckdb \
    -Wl,-rpath,"${LIB_DIR}" \
    -o "${OUT}"

echo ">> running C++ unit tests..."
"${OUT}"
