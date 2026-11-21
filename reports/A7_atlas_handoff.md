# A7 handoff: command production, executor shape and shader semantics

Evidence package for the independent Binary Atlas track. Checkable against the original SELF
with `tools/a5/verify_self.py`, which gates on image identity and runs 48 checks.

## Provenance

| Item | Value |
| --- | --- |
| Title | CUSA11919 |
| Image | 1.00, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Second image | 01.01, SHA-256 `8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75` |
| Size | 81,506,767 bytes, both |
| Source revision | `research/a7-command-flow-20261122` |
| Base | A6 `7195f39c` |
| Address space | inner-ELF VA; runtime VA = `module_base + RVA`, `module_base = 0x800000000` |

Both images pass all 48 checks. No address differs between them.

## Headline findings

**1. The command list is a singly linked list with a tail-slot pointer.** The append at
`0x13D6906` is four stores: `*tail = cmd`, `tail = &cmd->Next`, `cmd->Next = null`,
`cmd->vptr = table`. That is `FRHICommandListBase::AllocCommand` exactly.

**2. The executor walks a different structure than the list.** `0x14B00` walks an array of
command pointers at `+0x18` with a count at `+0x20`. The teardown at `0x14810` walks the same
array by slot. So the same command objects are reachable two ways: the list orders them, the
array stores them.

**3. `shader_modifier` rewrites the vertex shader's resource register.** The fourth argument to
`sceGnmSetVsShader` is masked into `SPI_SHADER_PGM_RSRC1_VS`. The valid bits are `0x030003c0`,
which are the VGPRS, SGPRS and priority/float-mode fields. So `shader-state + 0x1C0` changes the
shader's resource configuration, not just metadata.

## The command-list object

| Offset | Field |
| --- | --- |
| `+0x00` | head (`Root`) |
| `+0x08` | tail slot (`CommandLink`) |
| `+0x14` | command count |
| `+0x30` | bump allocator cursor |
| `+0x38` | bump allocator end |

Commands are bump-allocated. When a request would pass `+0x38`, the allocator calls `0x0E3EF80`
to grow.

## The `0x130` versus `0x138` reconciliation

A6 reported the `SetRenderTargets` command as `0x130` bytes and the allocator request as
`0x138`. Both are correct:

```
13d697a: leaq 0x130(%rax), %rcx   ; required end = cursor + 0x130  (payload size)
13d6981: cmpq 0x38(%rbx), %rcx    ; fits?
13d6987: movl $0x138, %esi        ; no -> grow by 0x138  (allocator hint)
```

`0x130` is the payload size the caller needs. `0x138` is the growth request, which is `0x130`
plus 8 bytes of slack so repeated small allocations do not each trigger a grow.

## The two structures

| Structure | Offset | Walked by | Order |
| --- | --- | --- | --- |
| Linked list | `+0x00` head, `+0x08` tail slot | append only | emission order |
| Pointer array | `+0x18` base, `+0x20` count | `0x14B00` executor, `0x14810` teardown | array index |

The executor also reads a per-command key at `cmd + 0x48` and calls `0x16830` when the pending
count exceeds a mask. The key's meaning is unresolved.

## Shader semantics

### The null PS array is the null-shader branch

A6 said the null array meant the registers were already written. That was wrong. The target
branches on the shader pointer:

```
ccd045: testq %rax, %rax
ccd048: je    0xccd08c          ; null -> null-shader branch
ccd04a: movq  0x1d8(%rax), %r12 ; non-null -> program data
ccd07e: addq  $0x8, %r12        ; array = program_data + 8
ccd0be: xorl  %edx, %edx        ; null path: array = null
```

The null path clears `SPI_SHADER_PGM_LO_PS`/`HI_PS` to zero, sets `DB_SHADER_CONTROL` to zero
and `CB_SHADER_MASK` to `0xF`. That is a deliberate no-pixel-shader state.

### The selector shifts a start offset, not selects an array

```
ccd103: movzbl 0x3(%r12), %ebx        ; selector
ccd12c: leaq   0x40(%r12,%rbx,4), %rdx ; base + selector*4
ccd131: movl   $0x1d, %esi             ; fixed 29 dwords
```

The pointer moves 4 bytes per selector step and the window size is fixed. So it is one
contiguous register block with a variable start, not N separate arrays.

The `+0x5E + selector*4` table is a parallel flag array, and bit 1 selects a variant path.

## Claim classification

### VERIFIED

- The append sequence and the list offsets.
- The bump allocator at `+0x30`/`+0x38`.
- The executor's array walk at `+0x18`/`+0x20`.
- The teardown's slot stride and `vtable[1]` call.
- The `0x130` payload versus `0x138` growth request.
- The null-shader branch and the non-null `program_data + 8` array.
- The selector arithmetic and the `+0x5E` flag table.
- The `shader_modifier` argument position.

### STRONG INFERENCE

- That the list is `FRHICommandListBase` and the tail slot is `CommandLink`. The shape matches
  the public header exactly, but the executable is stripped.
- That the array is a dispatch queue. It is walked by index and cleared after execution.
- That the selector picks a start offset into one contiguous block. The arithmetic supports it;
  the block's full extent was not measured.
- That the `+0x5E` bit 1 means "variant". The branch is visible; its purpose is not.

### UNRESOLVED

- The array population site. This is what would prove whether array order equals list order.
- The `cmd + 0x48` sort key.
- The thread that runs `0x14B00`.
- Where the shader-state object is populated.
- The full extent of the register block at `+0x40`.
- `BeginRenderPass` / `EndRenderPass`. Not found in the searched cluster.

## Scope correction on the A6 negative

A6 stated that `FRHICommandSetBoundShaderState` does not exist in this binary. That was too
broad. A6 searched the command vtable cluster around `0x03F26A00`, the shader-setter call sites,
and the pointer tables for the recorder and receiver. It did not enumerate every vtable, and the
trivial-destructor heuristic matches far more broadly than that cluster.

The correct statement: it is absent from the searched regions and from the path that reaches the
draw. A command elsewhere in the image is not excluded.

## What this does not claim

Ordering. The list is ordered by construction, but the executor consumes a different structure
whose population site was not traced. Whether array order equals list order is unproven, and no
observed sequence is asserted.

## Reproduction

```
# 48 checks against the original SELF; exits 2 on a non-approved image
python tools/a5/verify_self.py <eboot.bin>

# function bounds
python tools/a5/fde.py <unwrapped.elf> 0x14B00 0x14810 0x13D67E0
```

## Next targets, in priority order

1. Find the array population site. That is the single thing that would settle ordering.
2. Identify the `cmd + 0x48` sort key.
3. Determine the thread that runs the executor.
4. Trace where the shader-state object is populated.
5. Find the pass-delimiting mechanism.

## Files

| File | Purpose |
| --- | --- |
| `maps/A5_address_map.tsv` | machine-readable addresses, fields, confidence, including A7 additions |
| `reports/A7_phase1_command_flow.md` | append mechanism and the two-structure split |
| `reports/A7_phase2_shader_semantics.md` | null-shader branch, selector, RSRC1 modifier |
| `reports/A6_shader_binding.md` | the shader binding path, with the A7 scope correction |
| `tools/a5/verify_self.py` | 48 checks against the original SELF |
| `tools/a5/fde.py` | function bounds from `.eh_frame_hdr` |
