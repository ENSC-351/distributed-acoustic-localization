cmake_minimum_required(VERSION 3.28.0)
# Copyright (c) 2020 STMicroelectronics
#
# SPDX-License-Identifier: Apache-2.0
#

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(r5capture_firmware)

target_include_directories(app PRIVATE ${LIBMETAL_INCLUDE_DIR} ${OPENAMP_INCLUDE_DIR} ${PLATFORM_DIR})

# main_remote.c: Zephyr's OpenAMP resource-table sample (v4.5.0), unchanged.
# It provides the resource table remoteproc needs, RPMsg, and the shell over RPMsg.
target_sources(app PRIVATE src/main_remote.c src/heartbeat.c src/timer_fix.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
