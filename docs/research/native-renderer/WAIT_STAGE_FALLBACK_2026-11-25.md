# Native renderer: empty wait-stage fallback (2026-11-25)

## Evidence
Pinned NRI 06ff6f643777e72eda88bc6fb8d56267b85d872f, [QueueVK.hpp lines 75–110](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/QueueVK.hpp#L75-L110), [lines 121–154](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/QueueVK.hpp#L121-L154). The submit path removes unsupported pipeline stages from requested semaphore waits. On the legacy submission path, conversion to a zero legacy mask is replaced by BOTTOM_OF_PIPE. The synchronization2 path instead forwards the filtered mask. This is distinct from the previously discussed signal scope for retirement.

## Consequence (analysis, not reproduced)
A requested consumer stage that disappears during filtering does not automatically become a conservative whole-submission wait. A legacy bottom-of-pipe wait does not protect earlier consumer stages. Whether a particular NRI stage enum can reach this case needs further verification of the conversion and feature tables. The native semantic RHI should reject unsupported consumer scopes or explicitly choose a supported conservative all-commands wait; never treat an empty translated mask as proof of dependency coverage. Track enabled features, not merely physical-device support.

## Falsifiable test
Instrument vkQueueSubmit and vkQueueSubmit2. On an adopted device without synchronization2, submit a wait whose translated stage mask is empty; capture the legacy destination mask. Compare with a supported all-commands wait and with synchronization2 enabled. Use a producer timeline semaphore and a consumer copy to verify data; separately assert the captured scope, because successful readback alone cannot prove synchronization correctness. No GPU execution performed in this pass.

## Scope and provenance
Live foundation 6b359f520b9f41f854b2aac41bb74526f6c60e87; main e2f981c40cba7c8490177c20c179cb3608cc1442. Public branch fetch failed with `Failed to fetch restricted URL`; authenticated branch API succeeded. No game executable, assets, shadPS4 backend, or performance changes. This is a source-level finding for the standalone native renderer.