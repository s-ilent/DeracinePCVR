# NRI Vulkan external-image destruction contract — source audit

**Date:** 2026-11-23  
**Track:** Native PCVR Renderer Foundation; independent of shadPS4 and binary/live-hook tracks.  
**Status:** Pinned-source audit and focused static assertions; no Vulkan runtime, headset, or guest binary execution.  
**Repository base:** `research/native-nri-wrapped-image-barriers-20261123` at `b9ac659e8c5978f25255930f163a75a65ffb909c`; `main` independently checked at `e2f981c40cba7c8490177c20c179cb3608cc1442`.

## Question

The prior OpenXR swapchain-ownership report left an important unresolved issue: does destroying an NRI wrapper around a runtime-owned `VkImage` accidentally call `vkDestroyImage` or free Vulkan image memory? This pass checks the concrete implementation rather than relying on the wrapper API declaration.

## Pinned primary evidence

NVIDIA-RTX/NRI commit [`06ff6f643777e72eda88bc6fb8d56267b85d872f`](https://github.com/NVIDIA-RTX/NRI/tree/06ff6f643777e72eda88bc6fb8d56267b85d872f).

- [`Source/VK/TextureVK.hpp` lines 3–17](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L3-L17): `TextureVK::~TextureVK` gates all native-image and shared-memory destruction behind `if (m_OwnsNativeObjects)`. Inside that guard it calls `vmaDestroyImage` for VMA-backed images, otherwise `vk.DestroyImage`; shared-handle closure and `vk.FreeMemory` are also guarded.
- [`Source/VK/TextureVK.hpp` lines 76–129](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L76-L129): `Create(const TextureVKDesc&)` constructs the NRI texture description from supplied Vulkan metadata, then explicitly sets `m_OwnsNativeObjects = false` at line 126 and assigns the borrowed `VkImage` handle at line 127.
- [`Source/VK/TextureVK.hpp` lines 183–195](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L183-L195): `BindMemory` checks `m_OwnsNativeObjects` and rejects binding memory to a wrapped object.
- [`Source/VK/TextureVK.h` lines 52–60](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.h#L52-L60): ownership defaults to true for NRI-created textures; the sharing-mode field defaults false with an explicit TODO for wrapped-image sharing-mode uncertainty.
- [`Source/VK/DeviceVK.hpp` lines 728–743](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L728-L743): wrapping a supplied `VkDevice` sets device-level `m_OwnsNativeObjects = false`, and Vulkan instance/device destruction is guarded by this flag.

## Findings

**Verified in this pinned NRI implementation:** the `TextureVKDesc` wrapper path is non-owning. Destroying its `TextureVK` wrapper invokes the destructor but cannot enter the native `VkImage` destruction or memory-free block because `m_OwnsNativeObjects` was set false by the wrapper constructor. This resolves the precise external-image destruction question left open by the earlier OpenXR report **at source level**.

**Separately verified:** the Vulkan-device wrapper likewise does not destroy the externally supplied `VkDevice` or `VkInstance`. The ownership guard does not mean that NRI has no internal objects to clean up: queue wrappers, allocation bookkeeping, and other NRI-created resources still have their own lifetimes.

**Not proven:** the NRI wrapper's destructor being safe does not prove it is safe to destroy a wrapper while command buffers or descriptors still reference it, nor does it establish OpenXR's normative image-release synchronization. The previous sharing-mode/queue-family ambiguity is also unchanged. These are separate contracts.

## Native host lifecycle rule (proposed)

1. OpenXR/Vulkan bootstrap creates the runtime-compatible device and OpenXR swapchains.
2. Enumerate each swapchain's actual `VkImage` handles. For every image, create a non-owning NRI texture wrapper using the matching format, usage, dimensions, layers and sample count.
3. Key wrappers by `(swapchain generation, swapchain identity, image index)`; never use the wrapper pointer as a persistent resource identity.
4. On swapchain recreation or teardown, first stop new submissions and ensure all GPU work referring to these wrappers has completed; invalidate host descriptors/views and command references; then destroy NRI wrappers; finally destroy the OpenXR swapchains through the OpenXR host.
5. For the externally adopted device, destroy NRI's device wrapper only after NRI resources are released; destroy the underlying Vulkan/OpenXR objects according to the native bootstrap's own ownership contract.

This is a **proposed ordering**, not a claim that OpenXR requires a blanket `vkDeviceWaitIdle` on every frame. Normal frame synchronization and session teardown must be designed separately.

## Focused static checks

Read the pinned `TextureVK.hpp` through GitHub and evaluated four text assertions:
- Wrapped-image path sets `m_OwnsNativeObjects=false` before storing the supplied `VkImage`: **PASS**.
- Destructor encloses native-image destruction in the ownership conditional: **PASS**.
- `BindMemory` explicitly rejects wrapped textures: **PASS**.
- A naïve assertion that the destructor contains no `vk.DestroyImage` at all: **FAIL by design**. It is present for NRI-owned images, but guarded; a plain string absence check would be the wrong test.

These are static checks, not a compiled unit test or Vulkan validation run. The negative assertion is recorded to prevent overstating verification coverage.

## Architectural impact

The previous uncertainty about *whether NRI can safely wrap and later release OpenXR images without destroying them* is now resolved for this pinned revision. The remaining first-order risks for the native OpenXR host are **GPU completion and stale reference invalidation**, not accidental `vkDestroyImage` in the ordinary wrapper destructor. This does not justify modifying shadPS4, relying on its resource alias behavior, or claiming a portable Déraciné shader pipeline.

## Next falsifiable experiment

A small **host-only Vulkan/NRI wrapper-lifetime test** should create a Vulkan image outside NRI, wrap it using `CreateTextureVK`, destroy the NRI wrapper, and then successfully use and explicitly destroy the original image with Vulkan validation enabled. Repeat with an NRI-created image as a control, and separately exercise device-wrapper teardown while preserving the external `VkDevice`.

**Pass:** the external image/device remain valid after NRI wrapper destruction, NRI-owned control objects are destroyed normally, and validation reports no double-destruction or invalid access. **Fail:** a wrapped image/device is destroyed, freed, or otherwise invalidated by NRI teardown. **Inconclusive:** no compatible Vulkan device or validation layer available.

Do not test use-after-free on a live GPU queue; test stale-reference rejection through a host-side generation tracker instead. No copyrighted game assets, guest binary changes, or speculative FPS claims are involved.
