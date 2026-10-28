# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dl002)

target_sources(app PRIVATE src/main.c src/temperature.c src/ble.c src/hct6911.c src/pt100.c)
