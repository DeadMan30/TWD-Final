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

    add_executable(test_engine_meta_op_register
                   game/engine/tests/test_engine_meta_op_register.c)
    target_link_libraries(test_engine_meta_op_register PRIVATE twdgame)
    add_test(NAME engine_meta_op_register COMMAND test_engine_meta_op_register)

    add_executable(test_engine_meta_op_find
                   game/engine/tests/test_engine_meta_op_find.c)
    target_link_libraries(test_engine_meta_op_find PRIVATE twdgame)
    add_test(NAME engine_meta_op_find COMMAND test_engine_meta_op_find)

    add_executable(test_engine_mat4_identity
                   game/engine/tests/test_engine_mat4_identity.c)
    target_link_libraries(test_engine_mat4_identity PRIVATE twdgame)
    add_test(NAME engine_mat4_identity COMMAND test_engine_mat4_identity)

    add_executable(test_engine_handle_equals
                   game/engine/tests/test_engine_handle_equals.c)
    target_link_libraries(test_engine_handle_equals PRIVATE twdgame)
    add_test(NAME engine_handle_equals COMMAND test_engine_handle_equals)

    add_executable(test_engine_dlist_remove
                   game/engine/tests/test_engine_dlist_remove.c)
    target_link_libraries(test_engine_dlist_remove PRIVATE twdgame)
    add_test(NAME engine_dlist_remove COMMAND test_engine_dlist_remove)

    add_executable(test_engine_rbtree_rotate_right
                   game/engine/tests/test_engine_rbtree_rotate_right.c)
    target_link_libraries(test_engine_rbtree_rotate_right PRIVATE twdgame)
    add_test(NAME engine_rbtree_rotate_right COMMAND test_engine_rbtree_rotate_right)

    add_executable(test_engine_lua_settop
                   game/engine/tests/test_engine_lua_settop.c)
    target_link_libraries(test_engine_lua_settop PRIVATE twdgame)
    add_test(NAME engine_lua_settop COMMAND test_engine_lua_settop)

    add_executable(test_engine_lua_pushinteger
                   game/engine/tests/test_engine_lua_pushinteger.c)
    target_link_libraries(test_engine_lua_pushinteger PRIVATE twdgame)
    add_test(NAME engine_lua_pushinteger COMMAND test_engine_lua_pushinteger)

    add_executable(test_engine_lua_pushboolean
                   game/engine/tests/test_engine_lua_pushboolean.c)
    target_link_libraries(test_engine_lua_pushboolean PRIVATE twdgame)
    add_test(NAME engine_lua_pushboolean COMMAND test_engine_lua_pushboolean)

    add_test(NAME validate_functions
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/validate_functions.py)
    add_test(NAME source_files
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/source_files.py --check)
    add_test(NAME progress_check
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/progress.py --check-blocks)
    add_test(NAME verify_self_test
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/verify.py --self-test)
    add_test(NAME boot_harness
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/boot_harness.py --check)
endif()
