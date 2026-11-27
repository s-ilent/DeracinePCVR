# OpenXR image lease versus GPU completion — 2026-11-27

Scope: native host renderer only. No emulator integration. Foundation parent observed 48c0de3fe47ecbf5b351add535508eff14c18820; main e2f981c40cba7c8490177c20c179cb3608cc1442.

## Evidence
OpenXR-SDK-Source pinned 47c4761d05b27f884247480ac3fb8a6657907325.
- [openxr_program.cpp:865–889](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/47c4761d05b27f884247480ac3fb8a6657907325/src/tests/hello_xr/openxr_program.cpp#L865-L889): frame begin/end remain paired even when rendering is skipped.
- [openxr_program.cpp:959–983](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/47c4761d05b27f884247480ac3fb8a6657907325/src/tests/hello_xr/openxr_program.cpp#L959-L983): each eye has its own acquire/wait/render/release sequence.
- [graphicsplugin_vulkan.cpp:1593–1603](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/47c4761d05b27f884247480ac3fb8a6657907325/src/tests/hello_xr/graphicsplugin_vulkan.cpp#L1593-L1603): waits for previous command buffer use before reuse; only array index zero supported.
- [graphicsplugin_vulkan.cpp:1656–1669](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/47c4761d05b27f884247480ac3fb8a6657907325/src/tests/hello_xr/graphicsplugin_vulkan.cpp#L1656-L1669): submits current work and returns without an explicit post-submit CPU fence wait in the ordinary non-mirror path.

## Finding
The sample releases the current eye image after submission, not after an explicit CPU completion wait. The previous command-buffer completion wait is for reuse. This is implementation evidence, not normative permission for arbitrary queue or layout configurations. Separate eye swapchains are not evidence of multiview support.

## Proposed contract
Track image lease, submitted GPU work and completed GPU work separately. Keep resource retirement gated on completion. Do not introduce mandatory CPU waits at release solely on the basis of the sample's pre-recording wait. Verify normative Vulkan binding requirements before implementing asynchronous release.

## Next falsifiable experiment
Mock frame coordinator with delayed completion: assert acquire/wait precedes submission, release follows submission, end follows release, retirement waits for completion, and skipped frames submit zero layers. Then verify normative release layout and queue ordering and run a real Vulkan/OpenXR test. No runtime tests executed in this investigation.

Public branch fetch failed (Failed to fetch restricted URL); public source range fetch failed (DisabledError). Authenticated GitHub retrieval recovered branch heads and both source files.
