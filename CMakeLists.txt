cmake_minimum_required(VERSION 3.16)
project(tfu_rpg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(tfurpg_core STATIC
    core/src/ini.cpp
    core/src/model.cpp
    core/src/session.cpp)
target_include_directories(tfurpg_core PUBLIC core/include)
if(MSVC)
    target_compile_options(tfurpg_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(tfurpg_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    # The game is a 32-bit DirectX 9 executable: build with an x86 toolchain (see tools/build_windows.sh).
    add_library(tfu_rpg SHARED
        plugin/dllmain.cpp
        plugin/memory.cpp
        plugin/win_bridge.cpp
        plugin/overlay.cpp
        plugin/ui.cpp)
    target_link_libraries(tfu_rpg PRIVATE tfurpg_core d3d9)
    set_target_properties(tfu_rpg PROPERTIES PREFIX "" SUFFIX ".asi")
    if(MSVC)
        target_compile_options(tfu_rpg PRIVATE /W4 /utf-8)
    else()
        target_compile_options(tfu_rpg PRIVATE -Wall -Wextra)
        target_link_options(tfu_rpg PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    enable_testing()
    add_executable(tfurpg_tests tests/tests.cpp)
    target_link_libraries(tfurpg_tests PRIVATE tfurpg_core)
    add_test(NAME tfurpg_tests COMMAND tfurpg_tests ${CMAKE_SOURCE_DIR}/data)

    add_executable(tfurpg_sim tools/sim.cpp)
    target_link_libraries(tfurpg_sim PRIVATE tfurpg_core)
endif()
