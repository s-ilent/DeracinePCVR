# NRI queue submission fallback — 2026-11-27

Scope: native UE4-level host renderer only. Foundation HEAD before write: 1fe763adbe5994d7f094b98759999c11a04ca59c; main: e2f981c40cba7c8490177c20c179cb3608cc1442. Both retrieved via authenticated commit lookup after public branch URL failed with `Failed to fetch restricted URL`.

## Evidence
Pinned [QueueVK.hpp](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/QueueVK.hpp), 186 lines, retrieved in full. Queue creation selects a supplied shared lock or an internal lock (lines 2–12); submission takes that lock (around lines 65–70). In Submit, the synchronization2-disabled branch converts wait stages to legacy flags and attaches timeline values to VkSubmitInfo; signal stages are not carried into legacy submission. The synchronization2 branch instead retains per-semaphore stage masks in VkSubmitInfo2. See [submission implementation](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/QueueVK.hpp#L65-L175). Both submit without a VkFence. WaitIdle also takes the queue lock.

## Consequence (design proposal)
Use a backend-neutral whole-submission completion token, with ALL_COMMANDS signal scope on synchronization2; do not rely on early stage-specific signaling for resource retirement. Conservative wrapping can still use timeline values through legacy submission. The internal queue lock alone does not prove synchronization with external OpenXR or direct Vulkan callers. Initially serialize application queue operations on a single coordinator thread; verify runtime synchronization contract separately. No requirement for emulator GPU translation or alias workaround.

## Falsifiable test
On a host-owned timeline semaphore, submit a transfer followed by a dependent readback, signal completion, wait and compare bytes under legacy and synchronization2 modes. Include two submissions with increasing values and an externally owned semaphore lifecycle check. Require identical bytes and zero Vulkan validation errors. Verify stage mapping separately before using narrow wait scopes. No runtime tests performed; this is source analysis only. No game assets or executable changes.
