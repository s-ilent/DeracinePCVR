# NRI multiview validation boundaries — 2026-11-27

Foundation HEAD verified before write: `6b359f520b9f41f854b2aac41bb74526f6c60e87`; main `e2f981c40cba7c8490177c20c179cb3608cc1442`. Native renderer only; no emulator integration.

## Evidence
Pinned NRI `06ff6f643777e72eda88bc6fb8d56267b85d872f`, CommandBufferVal.hpp blob `515b07a741cf34dc96b1f6a07d5f83d2379648e5`.

- [BeginRendering, lines 740–754](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/Validation/CommandBufferVal.hpp#L740-L754): nonzero view mask rejected if reported maximum views is one or less. This makes conservative wrapping a hard validation gate, not merely a capability hint.
- [Full function, lines 740–870](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/Validation/CommandBufferVal.hpp#L740-L870): checks attachment types and resolve constraints, but does not explicitly check highest set view bit against maximum views or attachment layer range, nor compare pipeline view mask here. Descriptor is forwarded to backend. This is a local observation, not a claim that no other validation exists.
- [Resolve helper](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/Validation/CommandBufferVal.hpp#L212-L226): resolve requires multisampled source, single-sample destination and matching formats; simultaneous sharing is rejected.

## Proposed host contract
Validate highest set bit against device view limit and every participating attachment view's layer count; compare pipeline and rendering masks. Keep independent-eye path. Do not treat these as recovered UE4 requirements.

## Next experiment
With NRI and Vulkan validation enabled, test mask 0x3 with two-layer color/depth views, then deliberately use a one-layer view and mismatched pipeline mask. Host validator should reject negative cases before submission. Compare positive readback to direct Vulkan; test both render paths where supported. No runtime tests performed this run.

## Retrieval
Public foundation URL failed with `Failed to fetch restricted URL`; authenticated commit retrieval succeeded. Public source lookup failed with `DisabledError`; authenticated file retrieval succeeded after correcting a local argument-schema error (`repository_full_name` required). No game binaries or assets used.
