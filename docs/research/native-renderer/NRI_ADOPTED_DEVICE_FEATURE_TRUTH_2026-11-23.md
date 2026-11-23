# Native PCVR — Adopted Vulkan device feature truthfulness

**Date:** 2026-11-23. **Scope:** independent exact-revision source audit; no runtime or GPU test. **Branch:** `research/native-nri-multiview-pipeline-contract-20261123`; branched from foundation `6b359f520b9f41f854b2aac41bb74526f6c60e87`. Main independently observed at `e2f981c40cba7c8490177c20c179cb3608cc1442`.

## New question

Can an NRI wrapper infer which Vulkan features the **OpenXR-created/adopted VkDevice actually enabled** merely by querying the physical device? This is distinct from the already researched OpenXR image ownership, multiview mask, barriers and release contract.

## Evidence, pinned to NVIDIA-RTX/NRI `06ff6f643777e72eda88bc6fb8d56267b85d872f`

1. [`Include/Extensions/NRIWrapperVK.h:25-47`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L25-L47) says **all supported Vulkan 1.0 features must be enabled** on the supplied `vkDevice`. `DeviceCreationVKDesc` accepts existing instance/device/physical-device handles, enabled extension lists, queue families and `unknownFeatures`. The latter explicitly treats optional Vulkan 1.1+ features as disabled; even then, timeline semaphore and host-query-reset are required for some NRI operations.
2. [`Source/VK/DeviceVK.hpp:740-742`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L740-L742) marks a non-null external device as a wrapper. [Lines 1013-1018](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L1013-L1018) query **physical-device supported** features via `vkGetPhysicalDeviceFeatures2`, using the full pNext chain unless `unknownFeatures` is set.
3. [`Source/VK/DeviceVK.hpp:1050-1056`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L1050-L1056) uses those feature values to set internal dynamic-rendering support. [Lines 1570-1575](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L1570-L1575) derive `viewMaxNum` from physical multiview support; [lines 1608-1614](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L1608-L1614) expose `layerBasedMultiview` on the same basis.

**Finding:** `vkGetPhysicalDeviceFeatures2` reports *supported* features, not the `VkDeviceCreateInfo` feature-enable state of an already-created device. Therefore the wrapper's advertised optional capabilities can overstate what the OpenXR-host-created device actually enabled when `unknownFeatures=false`. This is a source-level **contract hazard**, not proof that any current OpenXR runtime violates it. `unknownFeatures=true` avoids assuming optional 1.1+ capabilities but also suppresses potentially valid multiview/dynamic-rendering paths and does **not** relax the all-supported-Vulkan-1.0-features requirement.

## Implication for a native OpenXR host

Do not construct the Vulkan device independently of the NRI adapter's requirements and then assume a successful NRI wrap proves feature compatibility. The host must retain the exact enabled-feature chain from its `vkCreateDevice` call, along with enabled extensions and queues, and compare that ledger to NRI's requirements. If the OpenXR Vulkan enable2 path creates the device through the host's `vkCreateDevice`, the host can construct the feature chain deliberately, subject to the runtime's required physical device and extension constraints. If the device was created elsewhere and the enabled-feature chain is unavailable, select a conservative supported adapter path or reject the configuration; physical-device capability queries alone cannot recover it.

**No assertion:** This pass did not establish which OpenXR runtime enables which Vulkan features, whether NRI's own validation catches the mismatch, or whether all NRI operations require the advertised features. No headset, driver, NRI build or Déraciné executable was exercised.

## Next falsifiable experiment

Create two Vulkan devices on the same capable physical GPU, deliberately enabling multiview on A and omitting it on B (with otherwise NRI-compatible Vulkan 1.0 features/extensions). Wrap both with NRI `unknownFeatures=false`, compare reported `layerBasedMultiview` and `viewMaxNum`, and attempt a two-view pipeline/render on both under Vulkan validation. Repeat B with `unknownFeatures=true`. Record the exact device feature chains and validation results. **Prediction:** NRI can advertise multiview for B despite it not being enabled; a multiview pipeline or rendering call on B will be invalid or rejected. If NRI rejects B at wrapping or disables multiview based on enabled state, the hypothesis is falsified.

## Architecture boundary

This is solely about host-created Vulkan/OpenXR device capability negotiation. It does not change the UE4-level resource/draw interception boundary, emulate GNM/PM4, modify shadPS4, or solve PS4 shader translation. No performance claim.
