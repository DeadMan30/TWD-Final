# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
project(twd_final C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /WX /fp:precise)
else()
    add_compile_options(-Wall -Wextra -Werror -ffp-contract=off -fno-fast-math)
endif()

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_subdirectory(runtime)
add_subdirectory(game)

if(NOT ANDROID)
    add_executable(test_runtime test/test_runtime.c)
    target_link_libraries(test_runtime PRIVATE twdrt)
    add_test(NAME runtime_abi COMMAND test_runtime)

    add_executable(test_engine_xorshift128_next
                   game/engine/tests/test_engine_xorshift128_next.c)
    target_link_libraries(test_engine_xorshift128_next PRIVATE twdgame)
    add_test(NAME engine_xorshift128_next COMMAND test_engine_xorshift128_next)

    add_test(NAME validate_functions
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/validate_functions.py)
    add_test(NAME source_files
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/source_files.py --check)
    add_test(NAME progress_check
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/progress.py --check-blocks)
    add_test(NAME verify_self_test
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/verify.py --self-test)
endif()
