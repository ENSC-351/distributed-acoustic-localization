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
# SPI to the MCP3208: "mcspi" (hardware MCU_SPI0, default) or "gpio" (bit-banged)
set(R5CAP_SPI mcspi CACHE STRING "SPI implementation: mcspi or gpio")
target_sources(app PRIVATE src/main_remote.c src/heartbeat.c src/timer_fix.c
                           src/hal_r5_timer.c src/hal_r5_${R5CAP_SPI}.c src/capture_main.c
                           ${CMAKE_CURRENT_SOURCE_DIR}/../fw/capture.c
                           ${CMAKE_CURRENT_SOURCE_DIR}/../fw/mcp3208.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../fw)
# The bit-banged build can start sampling on its own (no peripheral to power);
# the McSPI build waits for "r5cap start" after the board script checks MCU_SPI0.
if(R5CAP_SPI STREQUAL "gpio")
  target_compile_definitions(app PRIVATE AUTOSTART_RATE_HZ=4000)
endif()
