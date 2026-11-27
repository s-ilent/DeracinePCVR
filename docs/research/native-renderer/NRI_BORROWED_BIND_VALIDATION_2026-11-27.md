# Borrowed Vulkan texture binding validation — 2026-11-27

Foundation HEAD observed f7ac26b67647591df45dce524eb30149dffcf35e; main e2f981c40cba7c8490177c20c179cb3608cc1442. NRI pin 06ff6f643777e72eda88bc6fb8d56267b85d872f.

## Observation
The Vulkan texture wrapper passes true to TextureVal construction (DeviceVal.hpp 1604–1613). TextureVal.h 8–12 and 28–35 establish that this initializes the bound-state flag and that IsBoundToMemory returns it. DeviceVal.hpp 2046–2057 rejects already-bound textures before backend dispatch at 2078. Thus explicit BindTextureMemory through enabled NRI validation rejects a wrapped Vulkan image, even with wrapped memory (whose range checks are skipped at 2064–2075). This is a source-level result, not a runtime test. It does not establish protection in other allocation entry points or validation-disabled configurations.

Sources: https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/Validation/DeviceVal.hpp#L2046-L2087 and https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/Validation/DeviceVal.hpp#L1604-L1613 and https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/Validation/TextureVal.h#L8-L35.

## Proposed host contract and experiment
Reject borrowed-image allocation/binding in the portable RHI regardless of NRI validation settings. Test a synthetic externally allocated image: wrap, attempt explicit binding with validation enabled and verify INVALID_ARGUMENT and zero backend bind calls; test a mixed owned/borrowed batch and verify no backend dispatch; test host rejection with validation disabled without issuing invalid Vulkan calls. Inspect allocation helper dispatch separately. No GPU tests executed. No game assets, executable changes or emulator integration.

Retrieval: public branch request failed with Failed to fetch restricted URL; authenticated encoded branch URL returned HTTP 400 invalid repository path; literal slash branch URL succeeded. Main verified independently.
