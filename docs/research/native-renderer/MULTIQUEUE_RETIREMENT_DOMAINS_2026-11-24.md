# Multi-queue completion domains and resource retirement — 2026-11-24

## Scope and provenance
One bounded host-only follow-up to `TIMELINE_RETIREMENT_CONTRACT_2026-11-24.md`. GitHub main independently verified at `e2f981c40cba7c8490177c20c179cb3608cc1442` before creating isolated branch `research/native-multiqueue-retirement-20261124` from `research/native-timeline-retirement-contract-20261124`. No changes to main, the binary, or shadPS4.

## New question
Does one monotonic GPU-completion scalar safely retire resources used by two independently progressing queues?

## Executed host-only experiment
Compiled a C++17 model locally with `-Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer`. Output: `PASS 12 multi-queue ownership assertions`. Model stores each submission as `(queue_index, queue_local_signal_value, retained_resources)`, tracks independent `submitted[q]` and `completed[q]`, and releases only submissions for the queue whose completion advanced. Tested one resource shared by two queues, completion of queue 0 before queue 1, zero-progress and future-counter observations, a second submission on queue 0, and final retirement. The test source is committed alongside this report.

## Observed finding
When the same resource is held by submissions on two independent queues, completion of queue 0 does not authorize dropping the queue 1 reference. A single global completion scalar is insufficient *without an explicit synchronization protocol that orders all contributing queue submissions*. Queue-local completion values with independently retained submission references prevent premature release in this model.

This is a deterministic ownership-model result, **not** proof of Vulkan queue completion semantics. No Vulkan SDK, GPU, OpenXR runtime, or headset was used.

## Proposed native renderer contract
- Maintain a completion domain per Vulkan queue (or prove that submissions are serialized through one signaling domain).
- Each submission holds strong references to resources it touches until its own completion is observed; shared resources naturally survive while any queue retains them.
- A queue's timeline value may only advance when associated work is actually complete; CPU submission IDs are not GPU completion.
- Explicitly represent cross-queue ownership transfers, semaphore waits, and memory dependencies separately from reference retention. Keeping a resource alive does not make its GPU accesses synchronized.
- Reject regressive/future completion observations; treat device loss and failed submission as distinct state transitions, not synthetic successful completion.
- OpenXR runtime-owned image release, wrapper destruction, and queue ownership are separate from host resource retention.

## Next falsifiable experiment
On a Vulkan-capable host, use two queues (where available), two timeline semaphores, and a resource referenced by both submissions. Signal one queue first; verify the resource stays retained until the other queue's real semaphore counter reaches its submitted value. Enable validation layers; add explicit ownership transfer and queue-family tests if the queues belong to distinct families. On hardware exposing only one queue, test separate completion domains with a deterministic backend but do not claim Vulkan validation.

## Limitations
The C++ model does not exercise Vulkan submissions, memory barriers, device loss, or Déraciné's customized UE4 RHI. It is independent of shadPS4's graphics backend and its resource alias workaround. Non-rendering game/runtime compatibility remains a separate problem.
