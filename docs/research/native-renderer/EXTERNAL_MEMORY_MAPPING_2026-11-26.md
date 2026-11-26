# External Vulkan memory mapping ownership

Date: 2026-11-26. Static source inspection only; no GPU tests.

Foundation verified at 6b359f520b9f41f854b2aac41bb74526f6c60e87; main at e2f981c40cba7c8490177c20c179cb3608cc1442. Isolated branch from foundation.

## Evidence

Pinned NRI revision 06ff6f643777e72eda88bc6fb8d56267b85d872f, [MemoryVK.hpp lines 3–32](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/MemoryVK.hpp#L3-L32). The external-memory constructor checks memory type, marks the object non-owning, retains the supplied mapping pointer and offset, and calls vkMapMemory for host-visible memory if no pointer was supplied. The destructor exits immediately for non-owning objects, so it does not undo that mapping. Borrowed allocation ownership is therefore not equivalent to mapping-state ownership.

## Proposed contract

For host-visible borrowed allocations, require a host-managed persistent mapping and a valid pointer for the wrapped range; reject absent mapping in the adapter unless an explicit mapping lease is implemented. Host owns unmapping and freeing after all wrappers and GPU uses retire. Device-local borrowed memory needs no mapping. This is a policy proposal, not a recovered UE4 requirement. OpenXR swapchain images are borrowed images, not a reason to wrap runtime-owned memory.

## Falsifiable experiment

Create host-visible Vulkan allocation and test (A) pre-mapped pointer supplied, (B) unmapped allocation and null pointer, (C) already mapped allocation with null pointer. Trace MapMemory/UnmapMemory and wrapper destruction. A should not map again; B should map once and remain mapped after wrapper destruction; C should be rejected by the adapter before reaching NRI. Verify host cleanup and validation output. No GPU execution performed.

No emulator integration or executable changes. Public foundation URL failed with `Failed to fetch restricted URL`; authenticated commit retrieval succeeded. Public pinned implementation retrieval succeeded.
