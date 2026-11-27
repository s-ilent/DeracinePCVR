# Wrapped Vulkan device feature negotiation — 2026-11-28

Baseline research HEAD f36484acc390ae6049fad65ad108f2caf244ed59; main e2f981c40cba7c8490177c20c179cb3608cc1442, independently resolved via commit API. New file only.

## Evidence
NRI pinned revision 06ff6f643777e72eda88bc6fb8d56267b85d872f, [DeviceVK.hpp](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp). Lines 740–742 distinguish wrapper ownership. Lines 1011–1018 query physical device features: unknownFeatures wrappers query only core features, otherwise the full chain. Lines 1090–1127 check synchronization2 (exception for unknownFeatures) and reuse the supplied device instead of creating one. Lines 1574 and 1612 derive multiview limits and capability from features11.multiview. These queries establish physical support, not the actual logical-device enablement. Therefore a wrapper with known features can advertise multiview even if the creator omitted it; this is a source-derived risk, not a reproduced bug. Unknown-feature mode does not populate the Vulkan 1.1 multiview field through the query shown.

## Proposed host contract
Own and retain the exact enabled feature chain when creating the Vulkan device through OpenXR; intersect backend capabilities with actual enablement. Do not use unknownFeatures as a shortcut to multiview. Keep sequential stereo fallback. No changes to emulator or game runtime.

## Falsifiable test
On a multiview-capable physical device create two logical devices with identical extensions and synchronization2 enabled, differing only in multiview enablement. Wrap each with NRI, compare reported capabilities, and assert host rejects multiview on the disabled device before pipeline creation. Repeat unknownFeatures mode and record results. No GPU test executed this run.

## Retrieval
Public branch URL failed: Failed to fetch restricted URL; authenticated commit retrieval succeeded. Public source find failed: DisabledError; authenticated file retrieval initially failed schema validation ('repository_full_name' is a required property), corrected and retrieved all 4094 source lines. No copyrighted assets or executable modifications.
