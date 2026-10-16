# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

enable_language(C)

add_library(mdbx STATIC EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/src/libmdbx/mdbx.c
)

target_compile_definitions(mdbx
  PRIVATE
    MDBX_BUILD_SHARED_LIBRARY=0
    MDBX_BUILD_FLAGS="cmake"
    MDBX_WITHOUT_MSVC_CRT=0
    NDEBUG=1
    _GNU_SOURCE=1
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(mdbx PRIVATE
    -std=gnu11
    -O2
    -fvisibility=hidden
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
  )
endif()

target_include_directories(mdbx SYSTEM
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/libmdbx>
)

target_link_libraries(mdbx
  PRIVATE
    Threads::Threads
)

set_target_properties(mdbx PROPERTIES
  EXPORT_COMPILE_COMMANDS OFF
)
