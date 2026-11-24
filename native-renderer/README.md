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
    vk_bootstrap.{h,cpp}    instance, device, feature manifest, validation
    nri_adoption.{h,cpp}    NRI device adoption and image wrapping
    image_io.{h,cpp}        image creation, transitions, readback, clears
    shader_compile.{h,cpp}  GLSL to SPIR-V via glslangValidator
    diagnostics.{h,cpp}     status vocabulary and assertion helpers
  src/tests/
    t01_bootstrap.cpp
    t02_external_image.cpp
    t03_depth_stencil_aspects.cpp
    t04_stereo.cpp
    t05_submission_lifetime.cpp
```

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
