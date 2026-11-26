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

    add_executable(test_engine_lua_absindex
                   game/engine/tests/test_engine_lua_absindex.c)
    target_link_libraries(test_engine_lua_absindex PRIVATE twdgame)
    add_test(NAME engine_lua_absindex COMMAND test_engine_lua_absindex)

    add_executable(test_engine_lua_gettop
                   game/engine/tests/test_engine_lua_gettop.c)
    target_link_libraries(test_engine_lua_gettop PRIVATE twdgame)
    add_test(NAME engine_lua_gettop COMMAND test_engine_lua_gettop)

    add_executable(test_engine_lua_xmove
                   game/engine/tests/test_engine_lua_xmove.c)
    target_link_libraries(test_engine_lua_xmove PRIVATE twdgame)
    add_test(NAME engine_lua_xmove COMMAND test_engine_lua_xmove)

    add_executable(test_engine_lua_pushnil
                   game/engine/tests/test_engine_lua_pushnil.c)
    target_link_libraries(test_engine_lua_pushnil PRIVATE twdgame)
    add_test(NAME engine_lua_pushnil COMMAND test_engine_lua_pushnil)

    add_executable(test_engine_lua_pushnumber
                   game/engine/tests/test_engine_lua_pushnumber.c)
    target_link_libraries(test_engine_lua_pushnumber PRIVATE twdgame)
    add_test(NAME engine_lua_pushnumber COMMAND test_engine_lua_pushnumber)

    add_executable(test_engine_lua_pushunsigned
                   game/engine/tests/test_engine_lua_pushunsigned.c)
    target_link_libraries(test_engine_lua_pushunsigned PRIVATE twdgame)
    add_test(NAME engine_lua_pushunsigned COMMAND test_engine_lua_pushunsigned)

    add_executable(test_engine_lua_pushlightuserdata
                   game/engine/tests/test_engine_lua_pushlightuserdata.c)
    target_link_libraries(test_engine_lua_pushlightuserdata PRIVATE twdgame)
    add_test(NAME engine_lua_pushlightuserdata COMMAND test_engine_lua_pushlightuserdata)

    add_executable(test_engine_meta_member_find_str
                   game/engine/tests/test_engine_meta_member_find_str.c)
    target_link_libraries(test_engine_meta_member_find_str PRIVATE twdgame)
    add_test(NAME engine_meta_member_find_str COMMAND test_engine_meta_member_find_str)

    add_executable(test_engine_meta_member_find
                   game/engine/tests/test_engine_meta_member_find.c)
    target_link_libraries(test_engine_meta_member_find PRIVATE twdgame)
    add_test(NAME engine_meta_member_find COMMAND test_engine_meta_member_find)

    add_executable(test_engine_meta_vtable_move_construct
                   game/engine/tests/test_engine_meta_vtable_move_construct.c)
    target_link_libraries(test_engine_meta_vtable_move_construct PRIVATE twdgame)
    add_test(NAME engine_meta_vtable_move_construct COMMAND test_engine_meta_vtable_move_construct)

    add_executable(test_engine_meta_vtable_alloc
                   game/engine/tests/test_engine_meta_vtable_alloc.c)
    target_link_libraries(test_engine_meta_vtable_alloc PRIVATE twdgame)
    add_test(NAME engine_meta_vtable_alloc COMMAND test_engine_meta_vtable_alloc)

    add_executable(test_engine_meta_vtable_construct
                   game/engine/tests/test_engine_meta_vtable_construct.c)
    target_link_libraries(test_engine_meta_vtable_construct PRIVATE twdgame)
    add_test(NAME engine_meta_vtable_construct COMMAND test_engine_meta_vtable_construct)

    add_executable(test_engine_meta_vtable_copy_construct
                   game/engine/tests/test_engine_meta_vtable_copy_construct.c)
    target_link_libraries(test_engine_meta_vtable_copy_construct PRIVATE twdgame)
    add_test(NAME engine_meta_vtable_copy_construct COMMAND test_engine_meta_vtable_copy_construct)

    add_executable(test_engine_meta_vtable_destruct
                   game/engine/tests/test_engine_meta_vtable_destruct.c)
    target_link_libraries(test_engine_meta_vtable_destruct PRIVATE twdgame)
    add_test(NAME engine_meta_vtable_destruct COMMAND test_engine_meta_vtable_destruct)

    add_executable(test_engine_meta_vtable_delete
                   game/engine/tests/test_engine_meta_vtable_delete.c)
    target_link_libraries(test_engine_meta_vtable_delete PRIVATE twdgame)
    add_test(NAME engine_meta_vtable_delete COMMAND test_engine_meta_vtable_delete)

    add_executable(test_engine_lua_atpanic
                   game/engine/tests/test_engine_lua_atpanic.c)
    target_link_libraries(test_engine_lua_atpanic PRIVATE twdgame)
    add_test(NAME engine_lua_atpanic COMMAND test_engine_lua_atpanic)

    add_executable(test_engine_lua_pushthread
                   game/engine/tests/test_engine_lua_pushthread.c)
    target_link_libraries(test_engine_lua_pushthread PRIVATE twdgame)
    add_test(NAME engine_lua_pushthread COMMAND test_engine_lua_pushthread)

    add_executable(test_engine_lua_getctx
                   game/engine/tests/test_engine_lua_getctx.c)
    target_link_libraries(test_engine_lua_getctx PRIVATE twdgame)
    add_test(NAME engine_lua_getctx COMMAND test_engine_lua_getctx)

    add_executable(test_engine_meta_slot_init_ptr
                   game/engine/tests/test_engine_meta_slot_init_ptr.c)
    target_link_libraries(test_engine_meta_slot_init_ptr PRIVATE twdgame)
    add_test(NAME engine_meta_slot_init_ptr COMMAND test_engine_meta_slot_init_ptr)

    add_executable(test_engine_mat4_uniform_scale
                   game/engine/tests/test_engine_mat4_uniform_scale.c)
    target_link_libraries(test_engine_mat4_uniform_scale PRIVATE twdgame)
    add_test(NAME engine_mat4_uniform_scale COMMAND test_engine_mat4_uniform_scale)

    add_executable(test_engine_mat4_diagonal_scale
                   game/engine/tests/test_engine_mat4_diagonal_scale.c)
    target_link_libraries(test_engine_mat4_diagonal_scale PRIVATE twdgame)
    add_test(NAME engine_mat4_diagonal_scale COMMAND test_engine_mat4_diagonal_scale)

    add_executable(test_engine_mat4_from_translation
                   game/engine/tests/test_engine_mat4_from_translation.c)
    target_link_libraries(test_engine_mat4_from_translation PRIVATE twdgame)
    add_test(NAME engine_mat4_from_translation COMMAND test_engine_mat4_from_translation)

    add_executable(test_engine_mat4_from_scale_translation_vec
                   game/engine/tests/test_engine_mat4_from_scale_translation_vec.c)
    target_link_libraries(test_engine_mat4_from_scale_translation_vec PRIVATE twdgame)
    add_test(NAME engine_mat4_from_scale_translation_vec COMMAND test_engine_mat4_from_scale_translation_vec)

    add_executable(test_engine_pc_stubs
                   game/engine/tests/test_engine_pc_stubs.c)
    target_link_libraries(test_engine_pc_stubs PRIVATE twdgame)
    add_test(NAME engine_pc_stubs COMMAND test_engine_pc_stubs)

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
