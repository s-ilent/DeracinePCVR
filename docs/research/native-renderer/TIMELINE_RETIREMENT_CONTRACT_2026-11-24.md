# Monotonic GPU completion observations and resource retirement — 2026-11-24

## Scope / provenance
New bounded, host-only investigation extending the resource-retention prototype at commit `696005ad0281900c24523ddb6d900f53f28c48b4`. Verified GitHub `main` independently at `e2f981c40cba7c8490177c20c179cb3608cc1442` before creating isolated branch `research/native-timeline-retirement-contract-20261124` from that exact prior research commit. No modifications to main or other worktrees.

## New question
What minimum host-side validation is required before a completion counter can release deferred UE4 resource references, and which lifetime events are *not* equivalent to GPU completion?

## Observed executable experiment
Added `timeline_retirement_test.cpp`, a deterministic C++17 model. Compiled with:
```
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer timeline_retirement_test.cpp -o test
./test
```
Observed: `PASS 10 deterministic retirement checks` with ASan/UBSan enabled.

Evidence in the committed source:
- `RetirementQueue::submit`: assigns increasing values, retains submission-owned `shared_ptr` objects, rejects counter overflow (lines 13–18).
- `RetirementQueue::observe`: rejects decreasing or future completion values; releases all entries through the observed counter (lines 19–25).
- `abandonUnsubmitted`: discards recording-only references without pretending GPU completion (line 26).
- `teardownAfterDeviceIdle`: explicitly conditional on **external proof** that GPU work is idle; it does not query the device (line 28).
- Test body checks delayed release, earlier-versus-later submissions, duplicate observations, impossible future observations, regressive observations, discarded recordings, and teardown (lines 30–58).

## Finding / architectural consequence
**Observed in this model:** a monotonic completion value is a sufficient *index* for retiring a FIFO of submission-owned resources **only when the backend's signaling protocol guarantees that value means all associated earlier GPU work is finished**. Polling order may vary, but observed completed values must not regress. Recording discard and device teardown are separate lifetime transitions.

**Critical qualification:** the test supplies synthetic completion values. It does **not** validate Vulkan timeline semaphore semantics, submission signaling, queue ordering, multi-queue dependencies, GPU execution, device loss, or any UE4 PS4 binary behavior. In particular, `teardownAfterDeviceIdle()` would be unsafe if called without a successful backend idle/completion guarantee. The test intentionally has no Vulkan dependency.

## Native host contract proposed for validation
1. Assign a unique signal value to each graphics-queue submission **only after** the submission can be associated with a valid backend signal operation; handle failed `vkQueueSubmit2` without incorrectly retiring resources.
2. Poll the actual semaphore using `vkGetSemaphoreCounterValue` (or equivalent fence completion), never use CPU submission sequence numbers as completion evidence.
3. Keep submission-held resource ownership until the verified GPU completion value reaches its signal value. If multiple queues are introduced, track independent completion domains or explicit inter-queue dependencies; a single scalar counter is not automatically sufficient.
4. On device loss, do not call a fake idle-success path; define an explicit device-lost abandonment policy with device/driver object lifetime rules.
5. Maintain OpenXR swapchain release and wrapper destruction as separate contracts. This experiment neither acquires nor releases swapchain images.

## Next falsifiable experiment
A minimal Vulkan-only harness (no game, headset, or shadPS4): create a timeline semaphore, submit two command buffers with successive signal values, query `vkGetSemaphoreCounterValue`, and verify resource retention across incomplete/complete polls; test failed submission and teardown with validation layers. Pin the exact Vulkan headers/spec and loader versions in that experiment. Separately inspect exact UE4 4.18 source for command-list reference release timing before applying this model to recovered game interfaces.

## Constraints
No copyrighted assets or game binaries; no shadPS4 GPU/backend integration; no performance claims. This report documents tested host-side logic and explicit untested GPU assumptions, not a Vulkan or Déraciné compatibility result.
