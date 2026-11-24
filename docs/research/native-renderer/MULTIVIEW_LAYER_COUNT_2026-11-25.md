# Native multiview layer-count semantics — 2026-11-25

Scope: standalone UE4 semantic RHI, not shadPS4. Foundation 6b359f520b9f41f854b2aac41bb74526f6c60e87; main e2f981c40cba7c8490177c20c179cb3608cc1442, verified via authenticated commit retrieval.

## Observation
Pinned [NRI CommandBufferVK.hpp](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp): lines 1733–1775 derive attachment extent and forward view mask and layer count to dynamic rendering; lines 1799–1809 forward view mask and collect attachment extent for legacy rendering; lines 1930–1935 force legacy framebuffer layer count to one when multiview is active. Lines 1680–1684 likewise restrict clear rectangle layer count to one for multiview. Lines 1970–1974 retain rendering extent and mask separately.

## Design consequence
Do not infer eye count from framebuffer layer count or clear rectangle layer count. Preserve attachment view layer range, active view mask, and OpenXR projection subimage array index as separate fields. For conventional stereo array rendering, mask 3 selects views 0 and 1; a legacy framebuffer layer count of one does not imply mono rendering. A single-eye non-multiview fallback must instead select the corresponding attachment view and projection index. This is a proposed harness contract, not recovered PSVR ABI evidence.

## Falsifiable test
Render distinct colors using view index into a two-layer image, then clear both views and read each layer. Compare dynamic and legacy rendering, capture view mask, attachment view ranges, framebuffer layers and clear rectangle layers. Repeat with mask 0 and separate single-layer views. Require correct per-eye contents and zero validation errors. No GPU tests performed this run.

Public foundation retrieval failed: Failed to fetch restricted URL. Public source find failed: DisabledError. Authenticated retrieval succeeded for branch heads and pinned source. No game assets, executable modifications or emulator changes.
