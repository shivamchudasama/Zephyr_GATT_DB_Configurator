# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bulkxfer_client)

target_sources(app PRIVATE
   src/main.c
   src/app_log.c
)

target_include_directories(app PRIVATE
   src
   ${CMAKE_CURRENT_LIST_DIR}/../../../AppLog
)

# Client role only: this board hosts no BulkXfer service (no GATT_CB needed)
zephyr_compile_definitions(BLK_ENABLE_SERVER=0)

# Library sources (adds its *.c files to the app target)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. BulkXfer)
