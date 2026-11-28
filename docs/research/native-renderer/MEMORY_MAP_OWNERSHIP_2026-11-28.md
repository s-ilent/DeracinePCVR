# Borrowed Vulkan memory mapping lifetime — 2026-11-28

Foundation base: `5f8e43294306afa2bb834d1a8066dafb329525ed`; independently observed main: `e2f981c40cba7c8490177c20c179cb3608cc1442`.

## Evidence

NRI pinned revision `06ff6f643777e72eda88bc6fb8d56267b85d872f`, [MemoryVK.hpp lines 3–31](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/MemoryVK.hpp#L3-L31), retrieved as raw source (133 lines). The borrowed allocation constructor retains a supplied mapping pointer and offset; if no pointer is supplied for host-visible memory, it maps the native allocation itself. The destructor returns immediately for borrowed allocations, so it does not undo that mapping. This is a mapping-lifetime asymmetry, distinct from allocation ownership or flush-range handling. No runtime defect is asserted.

## Proposed host contract

For host-owned CPU-visible memory, maintain one authoritative mapping per VkDeviceMemory and supply the corresponding mapped pointer when wrapping. Track allocation ownership, mapping ownership, mapped interval and wrapper lifetime separately. Do not assume destroying a wrapper unmaps memory. If allowing NRI to initiate a mapping, explicitly coordinate its eventual unmapping with the host. Do not apply this policy to opaque OpenXR image allocations, which need not be exposed as memory objects.

## Falsifiable experiment

Capture Vulkan dispatch calls for a borrowed host-visible allocation with a supplied pointer (zero MapMemory calls expected), without a pointer (one MapMemory expected), and wrapper destruction (zero UnmapMemory and FreeMemory expected). Test rewrapping after destruction with the host mapping retained, and reject a second mapping attempt in the host policy. Inject map failure and verify no usable wrapper is published. Verify pointer/offset interpretation with nonzero offsets before implementing upload arenas.

## Status

Source inspection only; no runtime or GPU tests. Public foundation URL returned `Failed to fetch restricted URL`; authenticated commit retrieval resolved both heads. Isolated documentation branch; no main merge, executable modification, game assets, shadPS4 integration or performance claims.
