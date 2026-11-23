# Native renderer — OpenXR Vulkan bootstrap and error-boundary contract (2026-11-23)

## Scope and provenance

One bounded investigation: **what must the host establish before it hands a Vulkan device to NRI or NVRHI?** This is not a repeat of the image-ownership, multiview, barrier, queue-release or NVRHI state-reacquisition reports. No PS4 GPU translation or emulator modifications.

- Public source: KhronosGroup/OpenXR-SDK commit `24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8`, `include/openxr/openxr_platform.h` lines 461–535. [Pinned header](https://github.com/KhronosGroup/OpenXR-SDK/blob/24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8/include/openxr/openxr_platform.h#L461-L535).
- Previous branch baseline: `research/native-nvrhi-state-reacquire-20261123`, commit `52389b768285975a3181f5a3315dcd394918b08f`. Independently observed `main` at `e2f981c40cba7c8490177c20c179cb3608cc1442` before creating this branch.
- Method: exact-version header inspection. This report distinguishes the API's type-level guarantees from runtime-specific behavior and normative requirements that still need checking against the extension specification.

## New findings: the bootstrap boundary is a distinct interface

The `XR_KHR_vulkan_enable2` declarations explicitly define four operations:

| Operation | Inputs / outputs visible in pinned header | Implication |
|---|---|---|
| `xrGetVulkanGraphicsRequirements2KHR` | XR instance + system ID -> `XrGraphicsRequirementsVulkanKHR` | Host must obtain runtime graphics requirements, not assume arbitrary Vulkan compatibility |
| `xrCreateVulkanInstanceKHR` | XR instance + `XrVulkanInstanceCreateInfoKHR` -> `VkInstance`, `VkResult` | Host controls requested Vulkan instance create-info, runtime participates in creation |
| `xrGetVulkanGraphicsDevice2KHR` | XR instance + system ID + `VkInstance` -> `VkPhysicalDevice` | Device selection is tied to the XR system and the actual Vulkan instance |
| `xrCreateVulkanDeviceKHR` | XR instance + `XrVulkanDeviceCreateInfoKHR` -> `VkDevice`, `VkResult` | Device creation must use the selected physical device and explicit Vulkan create-info |

Evidence: [header lines 474–511](https://github.com/KhronosGroup/OpenXR-SDK/blob/24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8/include/openxr/openxr_platform.h#L474-L511).

### Two independent error channels

The two creation functions return `XrResult` **and** accept an output `VkResult*`. A native bootstrap API that reports only Vulkan success or only OpenXR success would discard an observable failure channel. Preserve both and avoid publishing partially created objects as ready. The header alone does not establish exact failure precedence or which outputs are valid on each failure; verify against the normative specification and test implementation.

### The graphics binding is not a device factory

`XrGraphicsBindingVulkan2KHR` is a typedef of `XrGraphicsBindingVulkanKHR` (line 495), whereas `XrVulkanDeviceCreateInfoKHR` separately carries `vulkanPhysicalDevice`, `pfnGetInstanceProcAddr`, `vulkanCreateInfo`, and optional allocator (lines 484–493). Thus the bootstrap and session-binding interfaces are distinct. The host must retain the actual instance/device/queue identity and pass a consistent binding to session creation; **do not allow the renderer abstraction to silently choose a different GPU**.

The SDK header defines the extension name `XR_KHR_vulkan_enable2` and spec version 4 at lines 463–466. The compile-time `#define XR_KHR_vulkan_enable2 1` is a header guard, **not evidence that the active runtime supports the extension**. Runtime extension enumeration and instance enablement are required host operations; exact ordering and compatibility checks remain to be validated against the normative extension text.

### Concrete host interface proposal (design, not implementation)

```cpp
struct XrVulkanBootstrap {
    XrInstance xr_instance;
    XrSystemId xr_system;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t graphics_queue_family;
    uint32_t graphics_queue_index;
    VkQueue graphics_queue;
};

struct VulkanCreateOutcome {
    XrResult xr_result;
    VkResult vk_result;
};
```

Keep this bootstrap owned by the OpenXR/Vulkan host. Pass a borrowed device descriptor to NRI/NVRHI; never let the rendering library independently create or replace the runtime-selected Vulkan device. Queue-family selection and actual `VkQueue` acquisition are **host design requirements**, not proved by the four typedefs alone. Require a validation step that compares the selected physical device, session binding and renderer-adopted device.

## Distinguish observed, inferred and untested

**Observed from pinned header:** four function-pointer signatures; their exact input structures; dual XR/Vulkan result channels for creation; alias of Vulkan2 session binding to the earlier Vulkan binding; compile-time extension name/version.

**Design inference:** isolate bootstrap as a host-owned module and expose an immutable device/queue identity to the renderer; fail closed on mismatches; preserve both result codes.

**Not verified:** runtime extension availability; required Vulkan instance/device extensions and feature negotiation; exact XR graphics requirements validation; physical device choice on a real headset; queue family compatibility; the normative failure-output semantics; whether NRI or NVRHI accepts the exact resulting device with the required features. No GPU code, headset or test was executed.

## Next falsifiable experiment

Implement a **standalone host-only bootstrap probe** (no Déraciné/shadPS4 dependency) using a pinned OpenXR loader and Vulkan headers:

1. Enumerate runtime extensions and require `XR_KHR_vulkan_enable2`; log a structured unsupported result otherwise.
2. Query the XR system's Vulkan graphics requirements, create instance via `xrCreateVulkanInstanceKHR`, obtain physical device via `xrGetVulkanGraphicsDevice2KHR`, create device via `xrCreateVulkanDeviceKHR`; record both result channels.
3. Record the actual physical device, graphics queue family/index and queue. Create an XR session with matching Vulkan graphics binding.
4. Attempt a deliberately mismatched physical-device/session binding in a disposable test and verify rejection, without modifying any game process.
5. Only after successful session bootstrap, adopt the same Vulkan handles through NRI and NVRHI in **separate** runs.

**Pass:** a real runtime accepts the session and the renderer-adopted device matches the runtime-selected Vulkan identity. **Fail/block:** runtime lacks enable2, rejects the requested feature/extension set, or session/device identity cannot be proven. Capture exact XR and Vulkan result codes.

No game executable, shader bytecode, private logs or copyrighted assets are included.
