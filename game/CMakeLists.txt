# SPDX-License-Identifier: GPL-2.0-only
file(GLOB_RECURSE TWD_GAME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp
)
# Exclude per-system test sources from the game library.
list(FILTER TWD_GAME_SOURCES EXCLUDE REGEX "/tests/")

add_library(twdgame SHARED
    ${TWD_GAME_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/game_init.c
    $<$<BOOL:${ANDROID}>:${CMAKE_CURRENT_SOURCE_DIR}/android_main.c>
)
target_include_directories(twdgame PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(twdgame PUBLIC twdrt)
