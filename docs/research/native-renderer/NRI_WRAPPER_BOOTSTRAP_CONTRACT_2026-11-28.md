# NRI wrapper bootstrap contract — 2026-11-28

Source: [NRIWrapperVK.h, pinned 06ff6f643777e72eda88bc6fb8d56267b85d872f, lines 17–35](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L17-L35). See also the complete DeviceCreationVKDesc declaration and its unknownFeatures comments in that file.

## Observation
The public wrapper contract requires all supported Vulkan 1.0 features to be enabled, a Vulkan minor version of at least 2, enabled extension lists and queue family descriptions. unknownFeatures is not a universal safe fallback: timeline semaphores and host query reset remain prerequisites for fence and query-reset operations, respectively; Vulkan 1.3 commands require extension entries in this mode. The wrapper descriptor does not expose a general enabled-feature chain.

## Consequence
Refine the previous wrapped-device feature report: a host capability intersection alone cannot make an arbitrarily configured device valid for NRI. Bootstrap must first satisfy the wrapper's baseline contract or select direct Vulkan. Preserve the actual enabled feature chain, extension list and queue inventory; do not infer enabled features from physical support. Treat the OpenXR device creation path as a negotiation step and reject unsupported combinations before wrapping. This is a proposed host policy, not recovered UE4 behavior.

## Falsifiable experiment
On one Vulkan 1.2+ physical device, compare a baseline device satisfying the wrapper contract with a device lacking timeline semaphore or host query reset enablement. The host preflight must reject the latter for the corresponding operations before NRI dispatch. Test unknownFeatures with and without the relevant extension entries; verify sequential stereo and fence/query operations on the valid configuration. No GPU tests performed in this run.

## Provenance
Foundation HEAD before write: f9d8df110fd79227958d14c50a7c89f236047bbf; main: e2f981c40cba7c8490177c20c179cb3608cc1442. Public branch fetch failed with `Failed to fetch restricted URL`; authenticated commit retrieval resolved both refs. No shadPS4 integration or game assets.
