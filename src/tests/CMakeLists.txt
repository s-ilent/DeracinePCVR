# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Find or download Google Test
include(FetchContent)
    FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

set(TEST_TARGETS
    shadps4_settings_test
    shadps4_gcn_test
    shadps4_guest_memory_validation_test
    shadps4_imgui_memory_test
    shadps4_libc_mspace_test
)

set(SETTINGS_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    test_emulator_settings.cpp
)

set(GCN_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_barriers.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_bitwise_conversion.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_composite.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_image.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_integer.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_logical.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_select.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_shared_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_special.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_undefined.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_warp.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_instructions.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_emit_context.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/format.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/decode.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/decode.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/data_share.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/export.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/translate.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/translate.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_alu.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_alu.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_interpolation.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/abstract_syntax_list.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/abstract_syntax_list.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/basic_block.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/basic_block.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/microinstruction.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/attribute.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/attribute.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/ir_emitter.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/ir_emitter.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/opcodes.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/opcodes.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/patch.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/patch.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/program.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/program.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ir_passes.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/constant_propagation_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/dead_code_elimination_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/identity_removal_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/resource_tracking_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/shader_info_collection_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ssa_rewrite_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/post_order.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/post_order.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/type.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/type.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/value.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/value.h
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/renderer_vulkan/liverpool_to_vk.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.h
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/resource.h

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/resource_tracking_pass_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/core_stub.cpp

    gcn/gcn_test_runner.hpp
    gcn/gcn_test_runner.cpp
    gcn/translator.hpp
    gcn/translator.cpp

    # Tests
    gcn/test_gcn_instructions.cpp
    gcn/test_geometry_inputs.cpp
    gcn/test_image_host_view.cpp
    gcn/test_pm4_stall_log.cpp

    # Vulkan-Hpp dispatch test (vkCmdBindVertexBuffers2)
    gcn/test_vk_dispatch_bindvb2.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/buffer_cache/vk_dispatch_reload.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/buffer_cache/vk_dispatch_reload.h
)

set(IMGUI_MEMORY_TEST_SOURCES
    imgui/test_memory_type_selection.cpp
)

set(GUEST_MEMORY_VALIDATION_TEST_SOURCES
    core/test_guest_memory_validation_cache.cpp
    core/test_pthread_stack_placement.cpp
    core/test_mouse_open_param.cpp
    core/test_flip_label_tracker.cpp
    core/test_fios2_module_choice.cpp
    video_core/test_legacy_vertex_attributes.cpp
    video_core/test_color_contract.cpp
    video_core/test_readconst_snapshot_diag.cpp
    video_core/test_fma_split_diag.cpp
    video_core/test_recip_refine_fix.cpp
)

set(LIBC_MSPACE_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/libraries/libc_internal/mspace.cpp
    core/test_libc_mspace.cpp
)

add_executable(shadps4_settings_test ${SETTINGS_TEST_SOURCES})
add_executable(shadps4_gcn_test ${GCN_TEST_SOURCES})
add_executable(shadps4_guest_memory_validation_test ${GUEST_MEMORY_VALIDATION_TEST_SOURCES})
add_executable(shadps4_imgui_memory_test ${IMGUI_MEMORY_TEST_SOURCES})
add_executable(shadps4_libc_mspace_test ${LIBC_MSPACE_TEST_SOURCES})

foreach(t ${TEST_TARGETS})
    target_include_directories(${t} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_compile_features(${t} PRIVATE cxx_std_23)
    target_compile_definitions(${t} PRIVATE BOOST_ASIO_STANDALONE)
endforeach()

target_link_libraries(shadps4_settings_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spdlog::spdlog
)
target_link_libraries(shadps4_gcn_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    magic_enum::magic_enum
    toml11::toml11
    Boost::headers
    Vulkan::Headers
    tsl::robin_map
    GPUOpen::VulkanMemoryAllocator
    sirit
    SDL3::SDL3
    spdlog::spdlog
    half::half
)

# vk_dispatch_reload.cpp must not be LTO-optimized (same as in the main binary)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/video_core/buffer_cache/vk_dispatch_reload.cpp
    PROPERTIES COMPILE_FLAGS "-fno-lto"
)
target_link_libraries(shadps4_imgui_memory_test PRIVATE
    GTest::gtest_main
    Vulkan::Headers
)
target_link_libraries(shadps4_guest_memory_validation_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    spdlog::spdlog
)
target_link_libraries(shadps4_libc_mspace_test PRIVATE
    GTest::gtest_main
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if (LIBCPP)
        foreach(t ${TEST_TARGETS})
            target_compile_options(${t} PRIVATE -fexperimental-library)
        endforeach()
    endif()
endif()

foreach(t ${TEST_TARGETS})
    if (WIN32)
        target_link_libraries(${t} PRIVATE onecore)
        target_compile_definitions(${t} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            NTDDI_VERSION=0x0A000006
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
        )
        if (MSVC)
            target_compile_definitions(${t} PRIVATE
                _CRT_SECURE_NO_WARNINGS
                _CRT_NONSTDC_NO_DEPRECATE
                _SCL_SECURE_NO_WARNINGS
                _TIMESPEC_DEFINED
            )
        endif()
    endif()
endforeach()

include(GoogleTest)

foreach(t ${TEST_TARGETS})
    gtest_discover_tests(${t}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DISCOVERY_TIMEOUT 60
    )
endforeach()

# ===========================================================================
# HTTP tests (libSceHttp - URI helpers and status-line parsing)
# ===========================================================================
# Self-contained block - mirrors the structure of the targets above so the
# new test does not depend on any earlier foreach iterations.

set(HTTP_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/libraries/network/http.cpp
    # Required to link RegisterLib's LIB_FUNCTION calls and the logger's
    # access to EmulatorSettings.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/loader_stub.cpp
    stubs/core_stub.cpp
    stubs/kernel_stub.cpp

    # Tests
    network/test_http_uri.cpp
    network/test_http_status_line.cpp
    network/test_http_parse_response_header.cpp
    network/test_http_lifecycle.cpp
)

add_executable(shadps4_http_test ${HTTP_TEST_SOURCES})

list(APPEND TEST_TARGETS shadps4_http_test)

target_include_directories(shadps4_http_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_http_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_http_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_http_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    magic_enum::magic_enum
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spdlog::spdlog
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if (LIBCPP)
        target_compile_options(shadps4_http_test PRIVATE -fexperimental-library)
    endif()
endif()

if (WIN32)
    target_link_libraries(shadps4_http_test PRIVATE onecore)
    target_compile_definitions(shadps4_http_test PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        NTDDI_VERSION=0x0A000006
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
    if (MSVC)
        target_compile_definitions(shadps4_http_test PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE
            _SCL_SECURE_NO_WARNINGS
            _TIMESPEC_DEFINED
        )
    endif()
endif()

gtest_discover_tests(shadps4_http_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Bachata Android runtime protocol tests
# ===========================================================================

set(BACHATA_RUNTIME_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/src/platform/bachata/runtime_client.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/bachata/controller_snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/bachata/audio_transport.cpp
    platform/test_bachata_runtime_client.cpp
    platform/test_bachata_controller_snapshot.cpp
)

add_executable(shadps4_bachata_runtime_test ${BACHATA_RUNTIME_TEST_SOURCES})

target_include_directories(shadps4_bachata_runtime_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_bachata_runtime_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_bachata_runtime_test PRIVATE
    FFmpeg::ffmpeg
    GTest::gtest_main
)

gtest_discover_tests(shadps4_bachata_runtime_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# libSceUsbd event-loop null-context safety (Driveclub FFB thread)
# ===========================================================================

add_executable(shadps4_usbd_test
    libraries/test_usbd_event_loop.cpp
)

target_include_directories(shadps4_usbd_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_usbd_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_usbd_test PRIVATE
    GTest::gtest_main
    libusb::usb
)

gtest_discover_tests(shadps4_usbd_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# libSceJpegDec HLE (Driveclub splash / packed JPEG textures)
# ===========================================================================

add_executable(shadps4_jpegdec_test
    libraries/test_jpegdec.cpp
    ${CMAKE_SOURCE_DIR}/src/core/libraries/jpeg/jpegdec.cpp
)
target_include_directories(shadps4_jpegdec_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/externals/stb
)
target_compile_definitions(shadps4_jpegdec_test PRIVATE JPEGDEC_STANDALONE_TEST)
target_compile_features(shadps4_jpegdec_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_jpegdec_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_jpegdec_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# AvPlayer buffering hysteresis (Driveclub attract video freeze)
# ===========================================================================

add_executable(shadps4_avplayer_buffering_test
    libraries/test_avplayer_buffering.cpp
)
target_include_directories(shadps4_avplayer_buffering_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_avplayer_buffering_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_avplayer_buffering_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_avplayer_buffering_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_avplayer_path_test
    libraries/test_avplayer_path.cpp
)
target_include_directories(shadps4_avplayer_path_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_avplayer_path_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_avplayer_path_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_avplayer_path_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_avplayer_read_test
    libraries/test_avplayer_read.cpp
)
target_include_directories(shadps4_avplayer_read_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_avplayer_read_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_avplayer_read_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_avplayer_read_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_pm4_type0_test
    video_core/test_pm4_type0.cpp
)
target_include_directories(shadps4_pm4_type0_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pm4_type0_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pm4_type0_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_pm4_type0_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_pm4_type1_test
    video_core/test_pm4_type1.cpp
)
target_include_directories(shadps4_pm4_type1_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pm4_type1_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pm4_type1_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_pm4_type1_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_pm4_packet_test
    video_core/test_pm4_packet.cpp
)
target_include_directories(shadps4_pm4_packet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pm4_packet_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pm4_packet_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_pm4_packet_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# FEX HLE stderr traces stay off unless BACHATA_FEX_HLE_TRACE is set
# ===========================================================================

add_executable(shadps4_hle_trace_test
    core/test_hle_trace.cpp
)
target_include_directories(shadps4_hle_trace_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_hle_trace_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_hle_trace_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_hle_trace_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Memory patch selection seam (Milestone 1)
# ===========================================================================

set(MEMORY_PATCHER_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    # io_file.cpp calls the ntdll entry points that ntapi.cpp defines.
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_memory_patcher.cpp
)

add_executable(shadps4_memory_patcher_test ${MEMORY_PATCHER_TEST_SOURCES})

target_include_directories(shadps4_memory_patcher_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_memory_patcher_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_memory_patcher_test PRIVATE
    BOOST_ASIO_STANDALONE
    MEMORY_PATCHER_TEST_OBSERVER
)

target_link_libraries(shadps4_memory_patcher_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_memory_patcher_test)

gtest_discover_tests(shadps4_memory_patcher_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Local repository v1 foundation (Milestone 2)
# ===========================================================================

set(PATCH_REPOSITORY_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_patch_repository.cpp
)

add_executable(shadps4_patch_repository_test ${PATCH_REPOSITORY_TEST_SOURCES})

target_include_directories(shadps4_patch_repository_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_patch_repository_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_repository_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_patch_repository_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_patch_repository_test)

gtest_discover_tests(shadps4_patch_repository_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Persistent per-game user state (Milestone 3)
# ===========================================================================

set(PATCH_STATE_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/patch_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_patch_state.cpp
)

add_executable(shadps4_patch_state_test ${PATCH_STATE_TEST_SOURCES})

target_include_directories(shadps4_patch_state_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_patch_state_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_state_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_patch_state_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_patch_state_test)

gtest_discover_tests(shadps4_patch_state_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Patch domain C ABI bridge (Milestone 4)
# ===========================================================================

set(PATCH_DOMAIN_SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/patch_domain.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
)

# Executable exercising the C ABI directly (no JVM involved).
add_executable(shadps4_patch_domain_test ${PATCH_DOMAIN_SOURCES}
    stubs/patch_domain_stub.cpp
    stubs/core_stub.cpp
    stubs/sdl_stub.cpp
    common/test_patch_domain.cpp
)

target_include_directories(shadps4_patch_domain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_patch_domain_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_domain_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_patch_domain_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
)

list(APPEND TEST_TARGETS shadps4_patch_domain_test)

gtest_discover_tests(shadps4_patch_domain_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# Host shared library exposing the same JNI symbols the Android .so will use, so the Kotlin
# end-to-end boundary test can drive the REAL native resolver from a JVM unit test.
add_library(shadps4_patch_domain_shared SHARED ${PATCH_DOMAIN_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/common/patch_domain_jni.cpp
    stubs/patch_domain_stub.cpp
    stubs/core_stub.cpp
    stubs/sdl_stub.cpp
)
set_target_properties(shadps4_patch_domain_shared PROPERTIES
    OUTPUT_NAME bachata_patch_domain
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/patch_domain
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/patch_domain
)
target_include_directories(shadps4_patch_domain_shared PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
    /usr/lib/jvm/java-17-openjdk-amd64/include
    /usr/lib/jvm/java-17-openjdk-amd64/include/linux
)
target_compile_features(shadps4_patch_domain_shared PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_domain_shared PRIVATE BOOST_ASIO_STANDALONE)
target_link_libraries(shadps4_patch_domain_shared PRIVATE
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
)

# ===========================================================================
# Managed launch session (Milestone 5)
# ===========================================================================

set(PATCH_SESSION_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_domain.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_patch_session.cpp
)

add_executable(shadps4_patch_session_test ${PATCH_SESSION_TEST_SOURCES})

target_include_directories(shadps4_patch_session_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_patch_session_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_session_test PRIVATE
    BOOST_ASIO_STANDALONE
    MEMORY_PATCHER_TEST_OBSERVER
)

target_link_libraries(shadps4_patch_session_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_patch_session_test)

gtest_discover_tests(shadps4_patch_session_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Preset resolution + Bloodborne local pilot (Milestone 6)
# ===========================================================================

set(PRESET_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_domain.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_patch_preset.cpp
)

add_executable(shadps4_preset_test ${PRESET_TEST_SOURCES})

target_include_directories(shadps4_preset_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_preset_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_preset_test PRIVATE
    BOOST_ASIO_STANDALONE
    MEMORY_PATCHER_TEST_OBSERVER
)

target_link_libraries(shadps4_preset_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_preset_test)

gtest_discover_tests(shadps4_preset_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Queued callable ownership (Common::UniqueFunction)
# ===========================================================================

add_executable(shadps4_unique_function_test
    common/test_unique_function.cpp
)

target_include_directories(shadps4_unique_function_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_unique_function_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_unique_function_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_unique_function_test)

add_executable(shadps4_quality_probe_test
    video_core/test_quality_probe.cpp
)

target_include_directories(shadps4_quality_probe_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_quality_probe_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_quality_probe_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_quality_probe_test)

gtest_discover_tests(shadps4_quality_probe_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

gtest_discover_tests(shadps4_unique_function_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Staged reads into GPU-tracked guest pages (Windows)
# ===========================================================================

add_executable(shadps4_staged_file_read_test
    core/test_staged_file_read.cpp
)

target_include_directories(shadps4_staged_file_read_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_staged_file_read_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_staged_file_read_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(shadps4_staged_file_read_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_staged_file_read_test)

gtest_discover_tests(shadps4_staged_file_read_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Autonomous-infrastructure devtools: virtual camera pacing and headless input
# ===========================================================================

add_executable(shadps4_virtual_camera_clock_test
    core/test_virtual_camera_clock.cpp
)

target_include_directories(shadps4_virtual_camera_clock_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_virtual_camera_clock_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_virtual_camera_clock_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_virtual_camera_clock_test)

gtest_discover_tests(shadps4_virtual_camera_clock_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_autopress_test
    core/test_autopress.cpp
)

target_include_directories(shadps4_autopress_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_autopress_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_autopress_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_autopress_test)

gtest_discover_tests(shadps4_autopress_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Guest backing writes spanning physically disjoint allocations
# ===========================================================================

add_executable(shadps4_backing_write_test
    common/test_backing_write.cpp
)

target_include_directories(shadps4_backing_write_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_backing_write_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_backing_write_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_backing_write_test)

gtest_discover_tests(shadps4_backing_write_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Texture cache subresource matching (ImageInfo::MipOf / SliceOf)
# ===========================================================================

add_executable(shadps4_image_subresource_test
    video_core/test_image_subresources.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/texture_cache/image_info.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/texture_cache/host_compatibility.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/tiling.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/renderer_vulkan/liverpool_to_vk.cpp
    ${CMAKE_SOURCE_DIR}/src/common/number_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    stubs/common_stub.cpp
    stubs/kernel_stub.cpp
    stubs/sdl_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
)

target_include_directories(shadps4_image_subresource_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_image_subresource_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_image_subresource_test PRIVATE NOMINMAX)

# Unreferenced constructors drag in the whole AMD GPU layer; keep only what the
# subresource tests call.
target_compile_options(shadps4_image_subresource_test PRIVATE /Gy /Gw)
target_link_options(shadps4_image_subresource_test PRIVATE /OPT:REF /OPT:ICF)

target_link_libraries(shadps4_image_subresource_test PRIVATE
    GTest::gtest_main
    Vulkan::Headers
    fmt::fmt
    spdlog::spdlog
    Boost::headers
    magic_enum::magic_enum
    tsl::robin_map
    half::half
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
)

list(APPEND TEST_TARGETS shadps4_image_subresource_test)

gtest_discover_tests(shadps4_image_subresource_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Guest mip chain clamping (AmdGpu::Image::NumLevels)
# ===========================================================================

add_executable(shadps4_mip_chain_test
    video_core/test_mip_levels.cpp
)

target_include_directories(shadps4_mip_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_mip_chain_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_mip_chain_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_mip_chain_test PRIVATE
    GTest::gtest_main
    Vulkan::Headers
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
    tsl::robin_map
    half::half
)

list(APPEND TEST_TARGETS shadps4_mip_chain_test)

gtest_discover_tests(shadps4_mip_chain_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Copy coverage: which mip levels a whole-image copy overwrites
#
# Header-only on purpose: the predicate reads ImageInfo fields and must be testable without a
# Vulkan device, because getting it wrong is how guest data is dropped.
# ===========================================================================

add_executable(shadps4_image_coverage_test
    video_core/test_image_coverage.cpp
)

target_include_directories(shadps4_image_coverage_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_image_coverage_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_image_coverage_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_image_coverage_test PRIVATE
    GTest::gtest_main
    Vulkan::Headers
    magic_enum::magic_enum
    fmt::fmt
    spdlog::spdlog
    Boost::headers
)

list(APPEND TEST_TARGETS shadps4_image_coverage_test)

gtest_discover_tests(shadps4_image_coverage_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Vulkan image reuse shape checks (pure metadata; no Vulkan device required)
# ===========================================================================

add_executable(shadps4_image_reuse_pool_test
    video_core/test_image_reuse_pool.cpp
)

target_include_directories(shadps4_image_reuse_pool_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/externals/vma/include
)
target_compile_features(shadps4_image_reuse_pool_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_image_reuse_pool_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_image_reuse_pool_test PRIVATE
    GTest::gtest_main
    Vulkan::Headers
    fmt::fmt
    spdlog::spdlog
)

list(APPEND TEST_TARGETS shadps4_image_reuse_pool_test)

gtest_discover_tests(shadps4_image_reuse_pool_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# GPU submission timing: the arithmetic behind SHADPS4_GPU_TIMING's numbers
#
# Header-only on purpose: the busy/gap rules decide what the frame statistics report and every
# way they can be wrong still reads as a plausible number, so they are tested without a device.
# ===========================================================================

add_executable(shadps4_gpu_timing_test
    video_core/test_gpu_timing.cpp
)

target_include_directories(shadps4_gpu_timing_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_gpu_timing_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_gpu_timing_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_gpu_timing_test PRIVATE
    GTest::gtest_main
    Vulkan::Headers
    magic_enum::magic_enum
    fmt::fmt
    spdlog::spdlog
    Boost::headers
)

list(APPEND TEST_TARGETS shadps4_gpu_timing_test)

gtest_discover_tests(shadps4_gpu_timing_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# C++ runtime hooks for libSceLibcInternal (guest static initialization)
# ===========================================================================

add_executable(shadps4_libc_cxa_test
    core/test_libc_cxa_hooks.cpp
    ${CMAKE_SOURCE_DIR}/src/core/libraries/libc_internal/libc_internal.cpp
    ${CMAKE_SOURCE_DIR}/src/core/libraries/libc_internal/libc_internal_cxa.cpp
    ${CMAKE_SOURCE_DIR}/src/core/loader/symbols_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/core/aerolib/aerolib.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    stubs/core_stub.cpp
    stubs/common_stub.cpp
    stubs/kernel_stub.cpp
    stubs/sdl_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/libc_internal_stub.cpp
)

target_include_directories(shadps4_libc_cxa_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_libc_cxa_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_libc_cxa_test PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    NTDDI_VERSION=0x0A000006
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_link_libraries(shadps4_libc_cxa_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
)

list(APPEND TEST_TARGETS shadps4_libc_cxa_test)

gtest_discover_tests(shadps4_libc_cxa_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Unreal guest memory snapshots (ps4-unreal-dumper layout)
# ===========================================================================

add_executable(shadps4_unreal_snapshot_test
    core/test_unreal_snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/snapshot.cpp
)

target_include_directories(shadps4_unreal_snapshot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_unreal_snapshot_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_unreal_snapshot_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_unreal_snapshot_test PRIVATE
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

list(APPEND TEST_TARGETS shadps4_unreal_snapshot_test)

gtest_discover_tests(shadps4_unreal_snapshot_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Unreal object inspector (read-only)
# ===========================================================================

add_executable(shadps4_unreal_inspector_test
    core/test_unreal_inspector.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/inspector.cpp
)

target_include_directories(shadps4_unreal_inspector_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_unreal_inspector_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_unreal_inspector_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_unreal_inspector_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_unreal_inspector_test)

gtest_discover_tests(shadps4_unreal_inspector_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Unreal inspector diagnostic report
# ===========================================================================

add_executable(shadps4_unreal_report_test
    core/test_unreal_report.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/inspector.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/report.cpp
)

target_include_directories(shadps4_unreal_report_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_unreal_report_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_unreal_report_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_unreal_report_test PRIVATE
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

list(APPEND TEST_TARGETS shadps4_unreal_report_test)

gtest_discover_tests(shadps4_unreal_report_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# OpenXR hand joint retargeting (pure math)
# ===========================================================================

add_executable(shadps4_hand_retarget_test
    core/test_hand_retarget.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/hand_retarget.cpp
)

target_include_directories(shadps4_hand_retarget_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_hand_retarget_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_hand_retarget_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_hand_retarget_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_hand_retarget_test)

gtest_discover_tests(shadps4_hand_retarget_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Live hand discovery: transform validation, component probe, override session
# ===========================================================================

add_executable(shadps4_hand_live_test
    core/test_hand_live.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/transform_validation.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/component_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/hand_override.cpp
    ${CMAKE_SOURCE_DIR}/src/core/unreal/hand_retarget.cpp
)

target_include_directories(shadps4_hand_live_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_hand_live_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_hand_live_test PRIVATE NOMINMAX)

target_link_libraries(shadps4_hand_live_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_hand_live_test)

gtest_discover_tests(shadps4_hand_live_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# OpenXR presentation size policy (pure math, no emulator dependencies)
# ===========================================================================
# Keeps the title's render size and the headset's picture size independent, and pins the acceptance
# sizes (2880x3240 / 3072x3456 / 3840x4320 in, 3036x3036 out) without a GPU.

add_executable(shadps4_vr_presentation_size_test
    test_vr_presentation_size.cpp
)

target_include_directories(shadps4_vr_presentation_size_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)

target_compile_features(shadps4_vr_presentation_size_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_vr_presentation_size_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_vr_presentation_size_test)

gtest_discover_tests(shadps4_vr_presentation_size_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# PSVR / OpenXR projection geometry (pure math, no emulator dependencies)
# ===========================================================================
# Proves when a full-rectangle UV resample of a guest eye image into an OpenXR
# projection view is geometrically exact, and quantifies the error when the source
# and destination frusta differ. Restored from e248372/e91f76c: the project keeps
# this suite in the tree because it guards projection math that later OpenXR
# changes could regress silently.

add_executable(shadps4_vr_projection_math_test
    test_vr_projection_math.cpp
)

target_include_directories(shadps4_vr_projection_math_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)

target_compile_features(shadps4_vr_projection_math_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_vr_projection_math_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_vr_projection_math_test)

gtest_discover_tests(shadps4_vr_projection_math_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
# ===========================================================================
# Scripted-input timeline (pure parsing/selection, no emulator dependencies)
# ===========================================================================
# A scripted line is timed either by the clock (the original behaviour) or by the guest's frame
# count, which is what makes an unattended run follow the title's frames. These rules decide when
# a line is in effect, so they are pinned without a GPU.

add_executable(shadps4_scripted_timeline_test
    test_scripted_timeline.cpp
)

target_include_directories(shadps4_scripted_timeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(shadps4_scripted_timeline_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_scripted_timeline_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_scripted_timeline_test)

gtest_discover_tests(shadps4_scripted_timeline_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# ImGui exit-save lifecycle (pure state, no ImGui/Vulkan dependency)
# ===========================================================================
add_executable(shadps4_imgui_exit_guard_test
    test_imgui_exit_guard.cpp
)

target_include_directories(shadps4_imgui_exit_guard_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(shadps4_imgui_exit_guard_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_imgui_exit_guard_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_imgui_exit_guard_test)

gtest_discover_tests(shadps4_imgui_exit_guard_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Déraciné UE4 RHI entry-probe contract and reversible patch planning (pure helpers)
# ===========================================================================
add_executable(shadps4_rhi_entry_probe_contract_test
    test_rhi_entry_probe_contract.cpp
)

target_include_directories(shadps4_rhi_entry_probe_contract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(shadps4_rhi_entry_probe_contract_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_rhi_entry_probe_contract_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_rhi_entry_probe_contract_test)

gtest_discover_tests(shadps4_rhi_entry_probe_contract_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Deracine vertex-input capture contract (pure helpers)
# ===========================================================================
add_executable(shadps4_vertex_input_capture_contract_test
    test_vertex_input_capture_contract.cpp
)

target_include_directories(shadps4_vertex_input_capture_contract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(shadps4_vertex_input_capture_contract_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_vertex_input_capture_contract_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_vertex_input_capture_contract_test)

gtest_discover_tests(shadps4_vertex_input_capture_contract_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Deracine RHI command-capture contract (pure helpers)
# ===========================================================================
add_executable(shadps4_rhi_command_capture_contract_test
    test_rhi_command_capture_contract.cpp
)

target_include_directories(shadps4_rhi_command_capture_contract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(shadps4_rhi_command_capture_contract_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_rhi_command_capture_contract_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_rhi_command_capture_contract_test)

gtest_discover_tests(shadps4_rhi_command_capture_contract_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Deracine shader-parameter capture contract (pure helpers)
# ===========================================================================
add_executable(shadps4_shader_param_capture_contract_test
    test_shader_param_capture_contract.cpp
)

target_include_directories(shadps4_shader_param_capture_contract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(shadps4_shader_param_capture_contract_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_shader_param_capture_contract_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_shader_param_capture_contract_test)

gtest_discover_tests(shadps4_shader_param_capture_contract_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Deracine draw-package schema and validator
# ===========================================================================
add_executable(shadps4_draw_package_test
    test_draw_package.cpp
)

target_include_directories(shadps4_draw_package_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(shadps4_draw_package_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_draw_package_test PRIVATE
    GTest::gtest_main
)

list(APPEND TEST_TARGETS shadps4_draw_package_test)

gtest_discover_tests(shadps4_draw_package_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
