# Native renderer — host-only UE4.18 semantic command recorder prototype

**Date:** 2026-11-24  
**Scope:** One bounded executable follow-up to [the public UE4.18 command ownership audit](../native-renderer/UE418_PARAMETER_COMMAND_OWNERSHIP_2026-11-23.md). This is a **proposed host semantic layer**, not a claim about Déraciné's binary ABI. No game executable, copyrighted asset, shadPS4 renderer, OpenXR runtime or GPU is involved.

## Source basis and revision

The earlier audit pinned public UE4.18.3 source to [aceyan/UE4.18.3_release @ `4c9fc5d3a2bf51029aa7d7295e09620d298739ef`](https://github.com/aceyan/UE4.18.3_release/tree/4c9fc5d3a2bf51029aa7d7295e09620d298739ef). [`RHICommandList.h` lines 516–630](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h#L516-L630) define typed deferred commands; [lines 1785–1867](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h#L1785-L1867) copy scalar parameter bytes but enqueue resource pointers. [Lines 1420–1490](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h#L1420-L1490) cover local uniform work-area semantics.

The prototype deliberately substitutes **generation keys** for raw resource pointers, a defensive *host design choice* rather than a behavior proven to exist in shipping UE4. It also models local layout lifetime explicitly.

## Implementation and executed checks

Added [`command_recorder_test.cpp`](command_recorder_test.cpp), a dependency-free C++17 test program with ordered `Constant`, `Texture`, `Local` and `Draw` events. Payload bytes are copied at record time. Texture and layout keys contain `{id,generation}`, validated at playback. This is a miniature semantic adapter, **not** a real RHI implementation.

Compiled and executed locally:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  command_recorder_test.cpp -o command_recorder_test
./command_recorder_test
# PASS 5 host-only semantic assertions
```

| Check | Stimulus | Observed result |
|---|---|---|
| Parameter snapshot | Record `[1,2,3,4]`, overwrite caller bytes with 9 | Recorded bytes remain `[1,2,3,4]` |
| Texture ABA | Record `(7,1)`, registry replaces with `(7,2)` | Stale reference rejected |
| Local uniform snapshot | Record 16 bytes, overwrite source | Recorded byte remains original |
| Layout ABA | Record layout `(8,1)`, registry replaces with `(8,2)` | Stale layout rejected |
| Ordered state | Constant 1, texture bind, constant 2, draw | Draw observes constant 2 and texture generation 2 |

**Test result:** 5/5 assertions passed with ASan/UBSan enabled. This verifies the host prototype's deliberately limited behavior, not upstream UE command execution or any Vulkan/OpenXR behavior.

## New engineering observation

A generation-aware registry can prevent *silent aliasing* after numeric handle reuse, but **validation at playback is not sufficient to preserve valid deferred UE4 semantics**. If a resource was legal when a command was recorded and released by the caller before the command executes, merely rejecting it is fail-safe but can incorrectly reject an otherwise valid UE command. A production bridge needs a separate **record-time retain/pin**, **command-list completion**, and **GPU completion** policy, plus distinct rules for command-list reset and cross-list resource references. These are hypotheses about the future bridge and are not established by this prototype.

The test also cannot model NRI descriptor lifetime or actual GPU access; its resource-generation namespace is intentionally independent of descriptor allocation.

## Next falsifiable experiment

Extend the harness with `retain(Key)`, `release(Key)`, `submit(epoch)`, and `complete(epoch)`, testing: (a) caller release after recording but before playback still permits a pinned resource; (b) same numeric ID cannot be recycled while a recorded command pins the old generation; (c) the final retain is released only after the chosen completion epoch; (d) reset/discard releases unsubmitted command references exactly once. Compare behavior with public UE4.18 resource ownership code before claiming compatibility. Only after that, attempt target-specific static identification of corresponding PS4 RHI methods in the original executable.

## Isolation and limitations

New branch `research/native-ue418-command-recorder-prototype-20261124`, based on the prior command-ownership research branch. Main was independently checked against `e2f981c40cba7c8490177c20c179cb3608cc1442` before writing and found identical. No merge requested. No GPU, headset, target binary, UE4 build, or external runtime was tested.
