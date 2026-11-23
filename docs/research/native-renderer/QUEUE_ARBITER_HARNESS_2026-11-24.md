# Host queue arbiter: concurrent NRI/OpenXR entry contract
Date: 2026-11-24

## Scope and provenance
One bounded, title-independent native-host prototype. Starting from `research/native-nri-xr-queue-submit-lock-20261124` in a new isolated branch. No changes to main, game binaries, shadPS4 graphics, PM4/GNM, or runtime presentation.

Prior exact-source audit: [NRI QueueVK.hpp at 06ff6f64, lines 82–170](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f64/Source/VK/QueueVK.hpp#L82-L170) holds an NRI-private lock around `vkQueueSubmit`/`vkQueueSubmit2`; [QueueVK.h at 06ff6f64, lines 16–49](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f64/Source/VK/QueueVK.h#L16-L49) exposes the internal lock. [OpenXR 1.1 Vulkan binding](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html) imposes external synchronization of session-bound Vulkan queue access for relevant frame/swapchain calls. These are inherited source observations, not new source claims.

## New executable investigation
Added `queue_arbiter_test.cpp`: a standalone C++17 host mutex wrapper around arbitrary callbacks, with a thread-local same-arbiter recursion guard and RAII lock release. Six concurrent producer threads (modeling NRI submission plus the four XR queue-touching calls and a second producer) each invoke 1,000 critical sections. Atomic counters detect overlapping callbacks. The harness also injects a callback exception and a nested call to the same arbiter.

Build and run on Linux:
```sh
c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread docs/research/native-renderer/queue_arbiter_test.cpp -o /tmp/queue_arbiter_test
/tmp/queue_arbiter_test
```

Observed output (local execution, 2026-11-24):
```
PASS: 6000 contended operations; zero overlap; exception unlock; recursive rejection; recovery
```
All assertions passed under AddressSanitizer and UndefinedBehaviorSanitizer. This is a stress test of the mock host-side mutual-exclusion contract, **not** a proof of Vulkan/OpenXR integration or a formal concurrency proof.

## Findings and design constraints
1. A host-owned, nonrecursive lock can serialize callback entry across competing producer threads. It must be shared by **every** host path that accesses the *same* session-bound `VkQueue`, not only the NRI submission path.
2. Callback failure must release the arbiter. A nested call to the same arbiter otherwise deadlocks with a plain nonrecursive mutex; the test rejects that case before acquiring the lock.
3. The critical section must not hold unrelated renderer/resource locks across XR calls without a documented global lock order. The prototype does not establish a safe lock order or callback reentrancy behavior for real runtimes.
4. Mutex serialization alone does **not** establish GPU memory barriers, cross-queue dependencies, image layouts, XR frame validity, or GPU completion. These remain separate contracts.
5. The harness does not validate queue identity. A different `VkQueue` handle requires explicit queue mapping and potentially a separate arbiter; OpenXR session queue family/index must be checked before NRI wrapping.

## Falsifiable next experiment
Use a real OpenXR-selected Vulkan queue and pinned NRI wrapper. Log/compare native queue handle, family and index. Wrap NRI queue submissions and the four XR frame/swapchain calls in the same arbiter. Clear an acquired image, submit with an ALL_COMMANDS timeline signal, release after successful submission without a CPU completion wait, and independently poll the timeline for resource retirement. Enable Vulkan validation and deliberately inject a failed submission. Pass criteria: matching queue identity, zero overlapping host critical sections, no validation errors, correct final layout, and no early retirement. Any mismatched queue identity or required out-of-lock queue access falsifies the current integration boundary.

## Limitations
No Vulkan device, headset, runtime, PS4 executable, UE4 RHI entry-point recovery, or NRI binary was exercised. This work addresses only the native host's queue-access concurrency, not non-rendering game/runtime systems.
