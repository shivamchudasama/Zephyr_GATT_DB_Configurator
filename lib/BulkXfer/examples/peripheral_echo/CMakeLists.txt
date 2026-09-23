# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bulkxfer_peripheral_echo)

target_sources(app PRIVATE
   src/main.c
   src/app_log.c
   src/BulkXfer_Service.c
)

target_include_directories(app PRIVATE
   src
   ${CMAKE_CURRENT_LIST_DIR}/../../../AppLog
)

# Library sources (each CMakeLists adds its *.c files to the app target)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../GATT_CB GATT_CB)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. BulkXfer)
