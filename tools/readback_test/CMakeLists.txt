# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Independent Vulkan readback regression test.
#
# Deliberately a separate CMake project: it links only the Vulkan loader and the C++ standard
# library, so it cannot acquire a dependency on shadPS4 or Deracine.
#
# Build:
#   cmake -S tools/readback_test -B build-readback-test
#   cmake --build build-readback-test

cmake_minimum_required(VERSION 3.20)
project(deracine_readback_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(deracine_readback_test main.cpp)

set(VULKAN_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/externals/vulkan-headers/include")
if(EXISTS "${VULKAN_HEADERS_DIR}/vulkan/vulkan.h")
    target_include_directories(deracine_readback_test PRIVATE "${VULKAN_HEADERS_DIR}")
endif()

find_library(VULKAN_LOADER vulkan-1)
if(VULKAN_LOADER)
    target_link_libraries(deracine_readback_test PRIVATE "${VULKAN_LOADER}")
elseif(WIN32)
    set(VULKAN_DEF "${CMAKE_BINARY_DIR}/vulkan-1.def")
    set(VULKAN_IMPLIB "${CMAKE_BINARY_DIR}/vulkan-1.lib")
    find_program(DUMPBIN_EXE dumpbin.exe)
    find_program(LIB_EXE lib.exe)
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
                target_link_libraries(deracine_readback_test PRIVATE "${VULKAN_IMPLIB}")
            endif()
        endif()
    endif()
endif()

if(MSVC)
    target_compile_options(deracine_readback_test PRIVATE /W4)
else()
    target_compile_options(deracine_readback_test PRIVATE -Wall -Wextra)
endif()
