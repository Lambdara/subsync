# SPDX-FileCopyrightText: 2026 Lambdara
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.16)
project(subsync C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Curses REQUIRED)

add_executable(subsync
    main.c
    tree.c
    ops.c
    ui.c
)

target_include_directories(subsync PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(subsync PRIVATE ${CURSES_LIBRARIES})
target_compile_definitions(subsync PRIVATE _DEFAULT_SOURCE _XOPEN_SOURCE=600)

target_compile_options(subsync PRIVATE -Wall -Wextra -Wpedantic)
