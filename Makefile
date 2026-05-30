PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
EXTENSION_NAME := mcap
EXTENSION_CONFIGS := $(PROJ_DIR)extension_config.cmake
EXT_NAME := mcap
EXT_CONFIG := $(PROJ_DIR)extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile
