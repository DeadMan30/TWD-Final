# SPDX-License-Identifier: GPL-2.0-only
add_library(twdrt STATIC
    hooks.c
    pal.c
    gfx_shim.c
    shader_cache.c
    subsystems.c
    win32_compat.c
    ttarch2.c
    audio_shim.c
    lua_vm.c
    d3dtx.c
    font.c
    scene.c
    d3dmesh.c
    savegame.c
    blowfish.c
    inflate.c
    android_entry.c
    $<$<BOOL:${ANDROID}>:platform_android.c>
    $<$<NOT:$<BOOL:${ANDROID}>>:platform_host.c>
)
target_include_directories(twdrt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(UNIX AND NOT ANDROID)
    find_package(Threads REQUIRED)
    target_link_libraries(twdrt PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(ANDROID)
    find_library(ANDROID_LIB android)
    find_library(LOG_LIB log)
    target_link_libraries(twdrt PUBLIC ${ANDROID_LIB} ${LOG_LIB})
endif()
