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

find_package(ZSTD QUIET)
find_package(zstd CONFIG QUIET)
find_package(lz4 CONFIG QUIET)

set(EXTENSION_SOURCES
    src/mcap_extension.cpp
    src/mcap_impl.cpp
    src/mcap_scan.cpp
    src/mcap_topics.cpp
    src/mcap_schemas.cpp
    src/schema_cache.cpp
    src/pushdown.cpp
    src/decoder.cpp
    src/views/rosout.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})

foreach(target ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    if(TARGET zstd::libzstd)
        target_link_libraries(${target} zstd::libzstd)
    elseif(TARGET ZSTD::ZSTD)
        target_link_libraries(${target} ZSTD::ZSTD)
    endif()
    if(TARGET lz4::lz4)
        target_link_libraries(${target} lz4::lz4)
    endif()
endforeach()

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
