# Deferred resource retention and submission epochs — 2026-11-24

## Scope and provenance

Independent host-only follow-up to `command_recorder_test.cpp` on `research/native-ue418-command-recorder-prototype-20261124`. Inspected that exact file via GitHub before starting. New branch `research/native-command-retention-epochs-20261124` was created from that research branch, not `main`. No PS4 binaries, graphics backend, shader translation, or shadPS4 work involved.

## Observation

The prior recorder stores texture references as `Key{id,gen}` and validates them during execution (`Registry::valid`, `Exec::run`). This correctly rejects recycled generations, but cannot represent the legitimate case where the caller drops its last reference *after recording* and *before execution*. The prior prototype has no retained ownership, submission epoch, completion epoch, or discard path.

## New host-only experiment

Added `resource_retention_test.cpp`, a standalone C++17 model using `shared_ptr` for retained resources, `weak_ptr` for registry observation, generation counters for identity recycling, command-list retention, and monotonically increasing submission/completion epochs.

The caller's release does not invalidate a recorded command; recycling the same numeric ID is prohibited until all retained owners are gone. Submission moves retention from the command list to an in-flight submission; completion retires submissions through the acknowledged epoch. Discarded command lists release retention without submission. A later submission's retained resource remains alive when an earlier epoch completes.

### Focused validation

Compiled locally with `c++ -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer` and executed successfully: **PASS 10 retention assertions**. Tests cover caller release, reuse rejection, in-flight retention, completion retirement, generation rejection, discard, and ordered completion of two submissions. This is a host-only ownership model, not a Vulkan or UE4 integration test.

## Architectural interpretation

**Observed in prototype:** Generation checks and retained ownership solve different problems. Generation checks reject stale handles; retention keeps legitimately referenced resources alive until work completes.

**Hypothesis for native UE4 bridge:** RHI command capture should acquire an owning resource reference at record time; submission transfers that ownership to an in-flight epoch; GPU completion releases it. A host resource registry must not recycle an identity while retained references exist.

**Not established:** UE4 4.18's precise reference-counting paths in the PS4-derived binary, when command-list translation should release CPU-side wrapper objects, and whether GPU backend resources require additional fence-dependent retention. This prototype deliberately models all resources conservatively through completion.

## Limitations and next falsifiable experiment

The mock completion API accepts a caller-provided epoch; it is **not** proof of GPU completion. Next: replace that API with a title-independent Vulkan timeline semaphore or fence-backed retirement queue, test out-of-order polling, discarded command lists, and forced device teardown. Separately recover the executable's UE4-level command-list and resource-reference semantics before claiming binary compatibility.

No game files, assets, performance claims, or executable modifications.
