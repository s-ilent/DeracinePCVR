cmake_minimum_required(VERSION 3.20)
project(deracine_native_harness LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Pinned external dependencies. NRI is consumed from a prebuilt tree so the
# harness does not re-download it; see README for the exact revision.
# ---------------------------------------------------------------------------
set(NRI_ROOT "" CACHE PATH "Path to the NRI source tree at the pinned revision")
set(NRI_BUILD_DIR "" CACHE PATH "Path to the built NRI tree (contains _Bin)")

if(NOT NRI_ROOT)
  message(FATAL_ERROR "NRI_ROOT must be set to the NRI source tree")
endif()
if(NOT NRI_BUILD_DIR)
  set(NRI_BUILD_DIR "${NRI_ROOT}/build")
endif()

set(NRI_INCLUDE_DIR "${NRI_ROOT}/Include")
set(NRI_LIB_DIR "${NRI_ROOT}/_Bin")

if(NOT EXISTS "${NRI_INCLUDE_DIR}/NRI.h")
  message(FATAL_ERROR "NRI.h not found under ${NRI_INCLUDE_DIR}")
endif()

find_package(Vulkan REQUIRED)

# The capture packages are JSON. Reuse the tree's own nlohmann header rather than
# adding a dependency.
set(JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH "nlohmann/json include dir")
if(NOT EXISTS "${JSON_INCLUDE_DIR}/nlohmann/json.hpp")
  set(_json_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/externals/json/single_include/nlohmann"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/externals/json/include/nlohmann"
  )
  foreach(_c ${_json_candidates})
    if(EXISTS "${_c}/json.hpp")
      # Strip the trailing /nlohmann so <nlohmann/json.hpp> resolves.
      get_filename_component(_parent "${_c}" DIRECTORY)
      set(JSON_INCLUDE_DIR "${_parent}")
      break()
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Shared support library: Vulkan bootstrap, NRI adoption, diagnostics.
# ---------------------------------------------------------------------------
add_library(harness_core STATIC
  src/common/vk_bootstrap.cpp
  src/common/nri_adoption.cpp
  src/common/diagnostics.cpp
  src/common/image_io.cpp
  src/common/shader_compile.cpp
  src/common/resource_registry.cpp
  src/backend/nri_device.cpp
  src/replay/package.cpp
  src/replay/tape.cpp
  src/replay/nri_tape_sink.cpp
)
target_include_directories(harness_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${NRI_INCLUDE_DIR}
  ${Vulkan_INCLUDE_DIRS}
  ${JSON_INCLUDE_DIR}
)
target_link_libraries(harness_core PUBLIC Vulkan::Vulkan)
target_compile_definitions(harness_core PUBLIC
  NRI_STATIC_LIBRARY=1
)

if(WIN32)
  target_compile_definitions(harness_core PUBLIC VK_USE_PLATFORM_WIN32_KHR=1 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# Tests. Each is a standalone executable that returns non-zero on failure.
# ---------------------------------------------------------------------------
set(HARNESS_TESTS
  t01_bootstrap
  t02_external_image
  t03_depth_stencil_aspects
  t04_stereo
  t05_submission_lifetime
  t06_semantic_nri_draw
  t07_resource_registry
  t08_lifetime_gpu
  t09_stale_resource_handles
  t10_recording_lifetime
  t11_multi_submission
  t12_expected_diagnostic_scope
  t13_p0_lifetime_corrections
  t14_textured_nri_draw
)

foreach(t ${HARNESS_TESTS})
  add_executable(${t} src/tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE harness_core)
  if(TARGET NRI_VK)
    add_dependencies(${t} NRI_VK)
  endif()
  target_link_libraries(${t} PRIVATE
    "${NRI_LIB_DIR}/NRI.lib"
    "${NRI_BUILD_DIR}/NRI_Shared.lib"
    "${NRI_BUILD_DIR}/NRI_VK.lib"
    "${NRI_BUILD_DIR}/NRI_Validation.lib"
  )
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# ---------------------------------------------------------------------------
# The capture replay executable. Consumes an existing SC-09H / SC-10S package.
# ---------------------------------------------------------------------------
add_executable(deracine_nri_replay src/replay/deracine_nri_replay.cpp)
target_link_libraries(deracine_nri_replay PRIVATE harness_core)
target_include_directories(deracine_nri_replay PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${NRI_INCLUDE_DIR}
  ${Vulkan_INCLUDE_DIRS}
  ${JSON_INCLUDE_DIR}
)
if(TARGET NRI_VK)
  add_dependencies(deracine_nri_replay NRI_VK)
endif()
target_link_libraries(deracine_nri_replay PRIVATE
  "${NRI_LIB_DIR}/NRI.lib"
  "${NRI_BUILD_DIR}/NRI_Shared.lib"
  "${NRI_BUILD_DIR}/NRI_VK.lib"
  "${NRI_BUILD_DIR}/NRI_Validation.lib"
)

add_executable(deracine_nri_tape src/replay/deracine_nri_tape.cpp)
target_link_libraries(deracine_nri_tape PRIVATE harness_core)
if(TARGET NRI_VK)
  add_dependencies(deracine_nri_tape NRI_VK)
endif()
target_link_libraries(deracine_nri_tape PRIVATE
  "${NRI_LIB_DIR}/NRI.lib"
  "${NRI_BUILD_DIR}/NRI_Shared.lib"
  "${NRI_BUILD_DIR}/NRI_VK.lib"
  "${NRI_BUILD_DIR}/NRI_Validation.lib"
)

enable_testing()
