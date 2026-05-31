cmake_minimum_required(VERSION 3.20)

set(TARGET_NAME mcap)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

find_path(
    MCAP_INCLUDE_DIR
    NAMES mcap/reader.hpp
    PATHS ${CMAKE_CURRENT_LIST_DIR}/mcap_repo/cpp/mcap/include
    NO_DEFAULT_PATH)
if(NOT MCAP_INCLUDE_DIR)
    find_path(MCAP_INCLUDE_DIR NAMES mcap/reader.hpp REQUIRED)
endif()
include_directories(${MCAP_INCLUDE_DIR})

# --- Locate lz4 and zstd (MCAP needs their headers to compile, and lz4 to link) ---
# Homebrew ships pkg-config files (not CMake config), so find_package(... CONFIG)
# silently finds nothing: headers go missing at compile time and symbols go
# unresolved at link time. Use pkg-config / find_library / find_path instead.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LZ4 QUIET liblz4)
    pkg_check_modules(PC_ZSTD QUIET libzstd)
endif()

# Headers: MCAP's reader.inl includes <lz4frame.h>, <zstd.h>, <zstd_errors.h>.
find_path(
    LZ4_INCLUDE_DIR
    NAMES lz4frame.h
    HINTS ${PC_LZ4_INCLUDEDIR} ${PC_LZ4_INCLUDE_DIRS})
find_path(
    ZSTD_INCLUDE_DIR
    NAMES zstd.h
    HINTS ${PC_ZSTD_INCLUDEDIR} ${PC_ZSTD_INCLUDE_DIRS})

# Library: only lz4 needs linking; zstd symbols come from DuckDB's bundled copy
# (linking Homebrew's zstd too would risk duplicate-symbol errors).
find_library(
    LZ4_LIBRARY
    NAMES lz4 liblz4 lz4_static
    HINTS ${PC_LZ4_LIBDIR} ${PC_LZ4_LIBRARY_DIRS})

if(NOT LZ4_INCLUDE_DIR OR NOT LZ4_LIBRARY)
    message(FATAL_ERROR "lz4 not found. Install it (e.g. 'brew install lz4') and re-run.")
endif()
if(NOT ZSTD_INCLUDE_DIR)
    message(FATAL_ERROR "zstd headers not found. Install zstd (e.g. 'brew install zstd') and re-run.")
endif()
message(STATUS "MCAP extension using lz4: ${LZ4_LIBRARY} (headers: ${LZ4_INCLUDE_DIR})")
message(STATUS "MCAP extension using zstd headers: ${ZSTD_INCLUDE_DIR}")
include_directories(${LZ4_INCLUDE_DIR} ${ZSTD_INCLUDE_DIR})

# --- Protobuf (dynamic message decoding of protobuf-encoded MCAP payloads) ---
# protobuf ships proper CMake config files (unlike lz4/zstd), so find_package works.
# It pulls in its abseil + utf8_range deps as imported targets. Add Homebrew keg
# prefixes to the search path so the config + its deps resolve without env setup.
if(APPLE)
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    foreach(dep protobuf abseil utf8-range)
        if(BREW_PREFIX AND EXISTS "${BREW_PREFIX}/opt/${dep}")
            list(APPEND CMAKE_PREFIX_PATH "${BREW_PREFIX}/opt/${dep}")
        endif()
    endforeach()
endif()
# Prefer protobuf's CMake config package (Homebrew, vcpkg, protobuf >=22 which
# pulls in abseil/utf8_range imported targets). Fall back to CMake's bundled
# FindProtobuf module (Debian/Ubuntu's libprotobuf-dev 3.21.x ships no config
# files). Both define the protobuf::libprotobuf imported target we link against.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf MODULE REQUIRED)
endif()
message(STATUS "MCAP extension using protobuf ${Protobuf_VERSION}")

set(EXTENSION_SOURCES
    src/mcap_extension.cpp
    src/mcap_impl.cpp
    src/mcap_scan.cpp
    src/mcap_topics.cpp
    src/mcap_schemas.cpp
    src/schema_cache.cpp
    src/pushdown.cpp
    src/decoder.cpp
    src/protobuf_decoder.cpp
    src/ros2_decoder.cpp
    src/views/rosout.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})

foreach(target ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    target_link_libraries(${target} ${LZ4_LIBRARY} protobuf::libprotobuf)
endforeach()

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
