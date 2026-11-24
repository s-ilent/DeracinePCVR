# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone diagnostic 3D replay.
#
# Deliberately a separate CMake project from the emulator: it links only the Vulkan loader and the
# C++ standard library, so it cannot accidentally acquire a dependency on shadPS4's renderer,
# scheduler, buffer cache, or guest address space.
#
# Build:
#   cmake -S tools/geometry_replay -B build-geometry-replay
#   cmake --build build-geometry-replay

cmake_minimum_required(VERSION 3.20)
project(deracine_scene_replay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(deracine_scene_replay main.cpp)
add_executable(deracine_textured_replay textured_replay.cpp)

# The semantic-tape replay. Same dependency discipline as the diagnostic replay: the Vulkan loader,
# the C++ standard library and a JSON parser, nothing from the emulator.
add_executable(deracine_tape_replay tape_replay.cpp tape.cpp draw_identity.cpp)

set(REPLAY_TARGETS deracine_scene_replay deracine_textured_replay deracine_tape_replay)

# The identity join is pure logic with no Vulkan dependency, so its tests live in the emulator's
# test tree, which already provides GTest. Keeping them out of this project means this project
# still links nothing but the Vulkan loader and the C++ standard library.

# Prefer the vendored headers when the tree has them, so the build does not depend on a system SDK.
set(VULKAN_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/externals/vulkan-headers/include")
if(EXISTS "${VULKAN_HEADERS_DIR}/vulkan/vulkan.h")
    foreach(tgt ${REPLAY_TARGETS})
        target_include_directories(${tgt} PRIVATE "${VULKAN_HEADERS_DIR}")
    endforeach()
endif()

# A real JSON parser rather than string search: a multi-stream package has similarly named fields
# in different objects, and confusing them silently would be worse than refusing the package.
set(JSON_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/externals/json/single_include")
if(EXISTS "${JSON_HEADERS_DIR}/nlohmann/json.hpp")
    foreach(tgt ${REPLAY_TARGETS})
        target_include_directories(${tgt} PRIVATE "${JSON_HEADERS_DIR}")
    endforeach()
else()
    message(FATAL_ERROR "nlohmann/json not found at ${JSON_HEADERS_DIR}")
endif()

# The loader is resolved at runtime from the system; only the import library is needed to link.
# On Windows the SDK does not ship vulkan-1.lib, so the build generates one from the system DLL's
# exports. That keeps the replay dependent on nothing but the Vulkan runtime that is already there.
find_library(VULKAN_LOADER vulkan-1)
if(VULKAN_LOADER)
    foreach(tgt ${REPLAY_TARGETS})
        target_link_libraries(${tgt} PRIVATE "${VULKAN_LOADER}")
    endforeach()
elseif(WIN32)
    set(VULKAN_DEF "${CMAKE_BINARY_DIR}/vulkan-1.def")
    set(VULKAN_IMPLIB "${CMAKE_BINARY_DIR}/vulkan-1.lib")
    find_program(DUMPBIN_EXE dumpbin.exe)
    find_program(LIB_EXE lib.exe)
    # The MSVC tools are not always on PATH, so fall back to the installed Build Tools location.
    if(NOT DUMPBIN_EXE)
        file(GLOB DUMPBIN_CANDIDATES
             "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe")
        if(DUMPBIN_CANDIDATES)
            list(GET DUMPBIN_CANDIDATES 0 DUMPBIN_EXE)
        endif()
    endif()
    if(NOT LIB_EXE)
        file(GLOB LIB_CANDIDATES
             "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/lib.exe")
        if(LIB_CANDIDATES)
            list(GET LIB_CANDIDATES 0 LIB_EXE)
        endif()
    endif()
    if(DUMPBIN_EXE AND LIB_EXE AND EXISTS "C:/Windows/System32/vulkan-1.dll")
        execute_process(
            COMMAND "${DUMPBIN_EXE}" /exports "C:/Windows/System32/vulkan-1.dll"
            OUTPUT_VARIABLE VULKAN_EXPORTS)
        string(REGEX MATCHALL "[0-9A-F]+ +([A-Za-z_][A-Za-z0-9_]*)" VULKAN_MATCHES "${VULKAN_EXPORTS}")
        set(VULKAN_NAMES "")
        foreach(match ${VULKAN_MATCHES})
            string(REGEX REPLACE "^[0-9A-F]+ +" "" name "${match}")
            list(APPEND VULKAN_NAMES "${name}")
        endforeach()
        if(VULKAN_NAMES)
            file(WRITE "${VULKAN_DEF}" "LIBRARY vulkan-1.dll\nEXPORTS\n")
            foreach(name ${VULKAN_NAMES})
                file(APPEND "${VULKAN_DEF}" "  ${name}\n")
            endforeach()
            execute_process(
                COMMAND "${LIB_EXE}" "/def:${VULKAN_DEF}" /machine:x64 "/out:${VULKAN_IMPLIB}"
                RESULT_VARIABLE LIB_RESULT)
            if(LIB_RESULT EQUAL 0)
    foreach(tgt ${REPLAY_TARGETS})
        target_link_libraries(${tgt} PRIVATE "${VULKAN_IMPLIB}")
    endforeach()
            endif()
        endif()
    endif()
    if(NOT VULKAN_IMPLIB)
        message(WARNING "vulkan-1 import library could not be generated")
    endif()
else()
    message(WARNING "vulkan-1 not found; the replay will fail to link")
endif()

if(MSVC)
    foreach(tgt ${REPLAY_TARGETS})
        target_compile_options(${tgt} PRIVATE /W4)
    endforeach()
        else()
    foreach(tgt ${REPLAY_TARGETS})
        target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    endforeach()
        endif()
