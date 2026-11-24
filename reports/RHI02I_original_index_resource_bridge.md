# RHI-02I: original UE4 index-resource identity and single-execution capture

## Result

The relationship between the original UE4 index-buffer object and the guest GPU allocation is
**statically recovered** from the indexed-draw implementation. A guarded observer implements it, the
matcher consumes it, and the two-execution capture design is replaced by a single-execution one.

**The mapping is not runtime verified.** No game launch was performed. The static layout is
consistent and the observer compiles, but no live record has been produced.

| Item | Value |
| --- | --- |
| Base | `25bd7d3a` (RHI-02) |
| Bridge tests | 16 passing |
| Selection tests | 10 passing |
| Existing identity tests | 24 passing, unchanged |
| Runtime mapping | **pending** |

## 1. Statically verified mapping

The indexed-draw implementation at `0x0CBB9C0` receives the index-buffer interface pointer in its
second argument. The resource base is that pointer minus `0x68`:

```
cbb9d7:  leaq  -0x68(%rsi), %rdx     ; rdx = resource base
cbb9db:  testq %rsi, %rsi
cbb9e1:  cmoveq %rsi, %rdx           ; null stays null
```

### The element width

```
cbb9eb:  cmpl  $0x0, 0x88(%rdx)      ; element-width indicator
cbb9f2:  setne %sil
cbb9f6:  leal  0x2(%rsi,%rsi), %esi  ; 2 or 4 bytes
cbb9fa:  imull %eax, %esi            ; x StartIndex
cbb9fd:  movq  %rsi, -0x40(%rbp)     ; byte offset
```

`+0x88` is zero for 16-bit indices and non-zero for 32-bit. The byte offset is
`StartIndex * element_width`, which confirms the RHI-01 finding that `StartIndex` is an index offset.

### The GPU address

```
cbbcf3:  addq  $0x20, %rbx           ; slot A
cbbcf9:  addq  $0x40, %rbx           ; slot B
cbbcfd:  movq  (%rbx), %rbx          ; the guest GPU address
cbbd3c:  addq  -0x40(%rbp), %rbx     ; + byte offset
cbbd4e:  movq  %rbx, %rcx            ; passed to sceGnmDrawIndex
cbbd55:  callq 0x33c81a0
```

**The address is not at a single fixed offset.** The implementation selects between `+0x20` and
`+0x40` by a thread-identity check at `0xcbbcce`-`0xcbbcf1`: it compares the current thread id
against a global record and takes `+0x20` when they match, `+0x40` otherwise.

Recording only one slot would be wrong for whichever thread took the other path. The observer reads
both and names the slot it used.

### What is verified and what is not

| Field | Offset | Evidence |
| --- | --- | --- |
| Interface pointer | argument 2 | **static verified** |
| Resource base | object − 0x68 | **static verified** |
| Element width | +0x88 | **static verified** |
| GPU address slot A | +0x20 | **static verified** |
| GPU address slot B | +0x40 | **static verified** |
| Byte offset | StartIndex × width | **static verified** |
| Allocation size | — | **not recovered** |
| Allocation offset | — | **not recovered** |
| Resource generation | — | **not recovered** |

The allocation size, the offset into a larger allocation, and any generation counter are **not**
recovered. The implementation does not read them at this call site, and inferring them from a
generic UE4 layout would be exactly the guess the task forbids. They remain unknown, and the record
carries explicit presence flags so a consumer can tell.

### The smallest additional observation required

To recover the allocation size and generation, the resource **creation** path must be found: the
constructor that allocates the guest buffer and writes the address into `+0x20`/`+0x40`. That is a
separate static investigation. Until it is done, an address match is the strongest available
identity, and a reused address cannot be distinguished from a genuine one.

## 2. Offline observer implementation

`Core::IndexIdentity` implements the recovered layout:

- Default OFF, gated on `SHADPS4_DERACINE_INDEX_IDENTITY=1`.
- Version-gated to the approved executable, by the same hash gate the other observers use.
- Bounded to 256 records.
- Read-only, using the existing `ReadGuestWord` helper, which validates the mapping before reading.
- Values copied by value; nothing retains a guest pointer.
- Independent of the package's downstream resource selection.

Each record carries an `execution_id` derived from the wall clock at install time. Its only job is
to make two records from different processes distinguishable, so a reused provenance id across runs
cannot be mistaken for the same draw.

The observer runs inside the existing RHI entry hook, which has already validated the guest
pointers, rather than installing a second hook.

## 3. Matcher integration

`JoinTapeToPackageWithBridge` is the form the capture should use. It compares:

`original UE4 RHI object → resolved guest allocation → package guest allocation`

The bridge is **required**, not optional. A join that fell back to comparing the object pointer
against the guest address when the bridge was absent would be the defect the module exists to
prevent, so an absent or incomparable bridge yields `InsufficientEvidence`.

The preserved classifications:

| Outcome | When |
| --- | --- |
| `Joined` | the bridge resolved and every comparable field agrees |
| `Mismatched` | a positive disagreement: different execution, address, width, or range |
| `InsufficientEvidence` | the bridge is absent, or carries a generation the package cannot confirm |

A generation the package cannot confirm is reported as insufficient evidence rather than assumed
equal, because an address match with an unconfirmed generation may be a reused allocation.

## 4. Single-execution capture

`Core::CaptureSession` replaces the two-launch design.

The previous procedure captured a package, read its provenance id, then launched the game again to
export a tape pinned to that id. A provenance id is a per-execution record ordinal, so the same id
in a second process names a different draw. The design could not establish what it claimed.

The replacement runs both observers in one execution and selects the compatible pair at export:

1. A shared provenance id is the **candidate lookup**.
2. The bridge must have resolved for that candidate.
3. The resolved guest base must equal the package's.
4. A different execution id is decisive and refuses the pair.

A candidate that fails is reported as `BridgeUnresolved` rather than replaced by another draw.

## 5. Tests

| Suite | Result |
| --- | --- |
| `shadps4_index_bridge_test` | 16 passed |
| `shadps4_capture_session_test` | 10 passed |
| `shadps4_draw_identity_test` | 24 passed, unchanged |

The bridge tests cover all fourteen required cases: a verified bridge, a different object resolving
to the same allocation, a generation mismatch, a reused address, a nonzero offset, a valid range, an
out-of-range access, a missing mapping, a disagreeing allocation, different executions, a reused
provenance id across executions, a synthetic tape carrying the package address directly, a positive
disagreement, and the guarantee that insufficient evidence is never joined.

A genuine match cannot be obtained by copying values between the fixtures: the tape carries an
object pointer and the package a guest address, and the join requires the bridge to resolve one into
the other.

## 6. Runbook

`docs/research/RHI02_same_draw_capture_runbook.md` is rewritten for one execution. It verifies the
executable hash, runs every observer in one process, explains session and provenance identity,
preserves the object-to-allocation evidence, gives the matching and validation commands, fails
clearly when the mapping is unresolved, keeps artifacts private, and stops before launching the game.

## 7. What remains

**Runtime mapping verification — pending.** No game launch was performed. The static layout is
consistent with the implementation, but no live record has been produced, so the claim that the
object resolves to the package's allocation is unproven.

**Allocation size and generation are unrecovered.** The creation path is the next static target.
Until it is found, an address match is the strongest identity available, and a reused address cannot
be distinguished from a genuine one.

**The observer has never run.** It compiles and is wired into the validated hook, but no record has
been produced.

## 8. Reproducible checks

```powershell
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_index_bridge_test
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_capture_session_test
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_draw_identity_test
.\build-tests\tests\shadps4_index_bridge_test.exe
.\build-tests\tests\shadps4_capture_session_test.exe
.\build-tests\tests\shadps4_draw_identity_test.exe
```

## 9. Scope

No game launch, no foregrounded process, no desktop capture, no SDK installation, no merge into
`main`. The observer is read-only, version-gated and default OFF.