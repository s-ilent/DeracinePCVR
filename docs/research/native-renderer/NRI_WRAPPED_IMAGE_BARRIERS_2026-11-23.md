# NRI wrapped-image barriers: queue-family metadata gap

**Date:** 2026-11-23. **Track:** Native PCVR Renderer Foundation. **Status:** pinned-source audit, no GPU/runtime execution. **Base:** `research/native-nri-external-image-ownership-20261123` @ `798329748eaced31d5c69557dec7483aa8b5d487`. **Main checked:** `e2f981c40cba7c8490177c20c179cb3608cc1442`. No changes to main or emulator.

## Question

Does NRI preserve the original Vulkan queue-sharing mode when wrapping an externally owned OpenXR swapchain `VkImage`, and what happens to Vulkan image barriers?

## Pinned source

NVIDIA-RTX/NRI `06ff6f643777e72eda88bc6fb8d56267b85d872f`:

- [`Source/VK/TextureVK.h` lines 53–60](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.h#L53-L60): `m_IsConcurrent = false` with explicit TODO: sharing mode unknown for wrapped images.
- [`Source/VK/TextureVK.hpp` lines 19–25](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L19-L25): NRI-owned image creation derives `m_IsConcurrent` from `VkImageCreateInfo.sharingMode`.
- [`Source/VK/TextureVK.hpp` lines 76–129](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L76-L129): wrapped-image creation imports image metadata, marks it non-owned, and stores the handle; it **does not populate sharing mode**.
- [`Source/VK/CommandBufferVK.hpp` lines 5–10](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L5-L10): queue family is `IGNORED` if queue is null or image is concurrent; `EXTERNAL_QUEUE` maps to `VK_QUEUE_FAMILY_EXTERNAL`.
- [`Source/VK/CommandBufferVK.hpp` lines 2678–2710](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L2678-L2710): texture barrier reads `textureVK.IsConcurrent()` and uses it to calculate both queue-family indices. Layout transitions, aspect masks, and layer/mip subranges are separately encoded.
- [`Source/VK/CommandBufferVK.hpp` lines 2720–2777](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L2720-L2777): legacy `vkCmdPipelineBarrier` fallback and synchronization2 path both consume the computed queue-family indices.

## Finding

**Verified from source:** an externally wrapped image defaults to `m_IsConcurrent=false`, even though its actual Vulkan creation sharing mode is unknown to NRI. The same barrier API will therefore emit queue-family indices for non-null source/destination queue references as if the image were exclusive. NRI-owned images do not have this ambiguity because NRI has their creation info.

**Important limitation:** this does *not* prove an OpenXR runtime supplies concurrently shared images, nor that normal same-queue layout transitions are invalid. For barriers with null queue references, `GetQueueFamilyIndex` returns `VK_QUEUE_FAMILY_IGNORED` regardless of sharing mode. Thus the immediate risk is **queue-family ownership-transfer semantics**, not every image barrier.

For a native OpenXR integration, do not assume NRI knows the sharing mode of runtime-created swapchain images, and do not request ownership transfers solely because NRI exposes queue objects. OpenXR acquire/wait/release and Vulkan synchronization are distinct responsibilities; this audit does not establish the normative OpenXR release-layout/completion contract.

## Proposed adapter policy (not implemented)

1. Treat externally owned image creation metadata as authoritative only when provided by the OpenXR runtime or applicable API contract. Do not guess its queue sharing mode.
2. Use NRI layout/access transitions with **null source and destination queue references** for the single-graphics-queue smoke-test path, producing `VK_QUEUE_FAMILY_IGNORED`. This avoids accidental ownership transfer without asserting the image's true sharing mode.
3. If multiple Vulkan queue families are required, first establish the actual sharing/ownership contract; use explicit Vulkan barriers at the host boundary where necessary, rather than pretending NRI's wrapped texture knows its original `VkImageCreateInfo`.
4. Maintain per-swapchain/image-index/layer layout state and acquire/wait/release state independently. Recreate all wrappers on swapchain generation changes.
5. Do not modify NRI or shadPS4 based on this source audit alone.

## Focused verification

Manual source-path check: wrapped `TextureVK::Create(TextureVKDesc)` never assigns `m_IsConcurrent`; field default is false; `CommandBufferVK::Barrier` reads that field; `GetQueueFamilyIndex` returns `IGNORED` for null queues. No local compilation or GPU execution occurred, so this is a **static, falsifiable source observation**, not a validation-layer result.

## Next falsifiable experiment

In a host-only Vulkan/NRI test with validation layers:

1. Create an **exclusive** Vulkan image and a **concurrent** Vulkan image (where two queue families are available), wrap each through NRI and record otherwise identical texture barriers with null queue references. Verify both emit `VK_QUEUE_FAMILY_IGNORED`.
2. Repeat with non-null queue references and capture `VkImageMemoryBarrier2` (or legacy equivalent). Verify NRI emits queue indices for both wrapped images; compare to the concurrent image's actual Vulkan sharing mode and expected no-transfer behavior.
3. Repeat on an actual OpenXR swapchain image **only after** checking the OpenXR/Vulkan ownership contract and runtime support. Test graphics-queue-only rendering first; do not invent queue transfers.
4. Pass if barrier fields match the predicted branch logic and validation stays clean in the single-queue path. Fail the hypothesis if wrapped-image creation propagates sharing mode or barriers ignore it contrary to the pinned source. Mark actual OpenXR interoperability inconclusive without a runtime/headset.

This isolates an NRI metadata limitation without claiming a Déraciné bug, portable shader execution, or any performance benefit.
