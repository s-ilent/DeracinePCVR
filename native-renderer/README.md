# Déraciné Native Renderer Harness

A standalone, game-asset-free Vulkan/NRI harness for validating the graphics
foundation that a native PC renderer for Déraciné's UE4.18-derived PSVR RHI will
target.

This is **not** a shadPS4 backend. It shares no code with the emulator's GPU
translation, texture cache, VR presenter or resource-aliasing workarounds.

## What it proves

Five executable tests, 68 assertions, all passing on an NVIDIA RTX 3090 Ti with
the Khronos validation layer active and zero validation errors.

| Test | What it establishes |
|---|---|
| `t01_bootstrap` | Vulkan device creation with explicit feature negotiation, NRI adoption of that device, native handle identity |
| `t02_external_image` | Vulkan images created outside NRI and imported without ownership transfer; contents verified by readback |
| `t03_depth_stencil_aspects` | Depth and stencil aspects are independently addressable; a combined attachment view does not imply stencil writes |
| `t04_stereo` | Sequential and multiview stereo into a two-layer image, with per-layer isolation verified |
| `t05_submission_lifetime` | Generation-tagged identities, deferred parameter snapshots, and GPU-completion-driven retirement via timeline semaphores |
| `t06_semantic_nri_draw` | **The end-to-end path**: an indexed stereo draw recorded through the semantic RHI, executed by NRI, into an externally owned image, verified per layer |
| `t07_resource_registry` | Deterministic CPU test of the corrected ownership model: app ownership and GPU retention are independent |
| `t08_lifetime_gpu` | The same lifetime semantics against real GPU completion, including deferred destruction and stale-id rejection |
| `t09_stale_resource_handles` | Stale ids are rejected with a structured reason and never reach a freed NRI object |
| `t10_recording_lifetime` | A recording owns its resources; views retain their parent textures; external images survive wrapper destruction |
| `t11_multi_submission` | Multiple submissions outstanding at once, with a reusable command-context pool and backpressure |
| `t12_expected_diagnostic_scope` | A closed expectation cannot be satisfied by a later unrelated validation error |

## Dependencies

Pinned exactly:

- **NRI** `06ff6f643777e72eda88bc6fb8d56267b85d872f`
- **Vulkan SDK** 1.4.363.0 (headers, loader, `glslangValidator`)
- **CMake** >= 3.20, **Ninja**, **Clang** 23.1.5

## Build

```
git clone https://github.com/NVIDIA-RTX/NRI.git _deps/NRI
cd _deps/NRI && git checkout 06ff6f643777e72eda88bc6fb8d56267b85d872f
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNRI_ENABLE_D3D11_SUPPORT=OFF -DNRI_ENABLE_D3D12_SUPPORT=OFF \
  -DNRI_ENABLE_VK_SUPPORT=ON -DNRI_ENABLE_NONE_SUPPORT=OFF \
  -DNRI_STATIC_LIBRARY=ON
cmake --build build

cd ../../native-renderer
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNRI_ROOT=<path-to>/_deps/NRI
cmake --build build
```

## Run

```
cd native-renderer/build
./t01_bootstrap
./t02_external_image
./t03_depth_stencil_aspects
./t04_stereo
./t05_submission_lifetime
```

Each returns 0 on success and prints a per-assertion log. Validation errors are
counted and reported; a non-zero error count fails the test.

## Layout

```
native-renderer/
  CMakeLists.txt
  src/common/
    vk_bootstrap.{h,cpp}      instance, device, feature manifest, validation
    nri_adoption.{h,cpp}      NRI device adoption and image wrapping
    image_io.{h,cpp}          image creation, transitions, readback, clears
    shader_compile.{h,cpp}    GLSL to SPIR-V via glslangValidator
    diagnostics.{h,cpp}       status vocabulary, expected diagnostics
    resource_registry.{h,cpp} corrected ownership model
    semantic_rhi.h            backend-independent interface
  src/backend/
    nri_device.{h,cpp}        the NRI implementation of semantic_rhi.h
  src/tests/
    t01_bootstrap.cpp
    t02_external_image.cpp
    t03_depth_stencil_aspects.cpp
    t04_stereo.cpp
    t05_submission_lifetime.cpp
    t06_semantic_nri_draw.cpp
    t07_resource_registry.cpp
    t08_lifetime_gpu.cpp
```

## Ownership model

A resource has several independent reasons to stay alive: the application still
owns it, a command buffer being recorded references it, submitted GPU work
references it, and a dependent resource needs it. Physical destruction happens
only when all of them are gone.

`Destroy()` revokes application ownership and defers physical destruction while
references remain. A pending-destroy resource rejects new work while previously
recorded commands finish safely.

The registry is the single authority for resource identity. Every lookup
validates index, generation, type and lifecycle state, so a stale id can never
reach a freed backend object. Backend payloads live behind the registry and are
deleted by its destruction callback.

## Command contexts

Four reusable command contexts, each with its own allocator and command buffer.
A context is only reset once its command buffer is known to have completed, so
an allocator is never reset while pending. When all contexts are in flight,
`BeginCommands` waits for the oldest submission. There is no per-frame
`vkQueueWaitIdle`.

## Status vocabulary

Every assertion is one of: implemented, compiled, executed, verified,
unsupported, blocked, failed. A test that cannot run on the current device
reports `UNSUPPORTED` rather than passing silently.

## Known limitations

- Windows-only build at present. The Vulkan code is portable; the shader
  compiler invocation and temp-path handling are Win32-specific.
- No OpenXR executable yet. The loader is present (SteamVR) but the OpenXR SDK
  headers are not installed. See the report for the exact blocker.
- No software-Vulkan (lavapipe) CI path yet.
