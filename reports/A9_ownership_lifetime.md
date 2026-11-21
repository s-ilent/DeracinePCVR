# A9: executor ownership, threading and command lifetime

## Result

The executor at `0x13764D0` is gated by **thread identity**, and it **does not run command
destructors**. It resets the allocator in place.

That second point is the one that matters for any future capture: command payloads are valid
only during the walk, and the memory is reclaimed wholesale afterwards.

## Ownership

### The command list is a global, not a member

Two globals hold command lists:

| Global | Role |
| --- | --- |
| `0x47C07D0` | primary command list |
| `0x47C07D8` | secondary command list |

The executor reads them at `0x1376593` and `0x13765DF`. They are cleared to null after use, so
they are borrowed pointers with a short lifetime, not owned members.

### The flush gate

`0x1376110` is the gate that decides whether a list is flushed:

```
1376110: movq 0x47c07c8, %rax        ; a global state object
137611b: cmpl $0x0, 0x4(%rax)        ; a count
137611f: jle  0x13761bc              ; nothing pending -> call 0x13761d0
1376125: movq 0x47c07d0, %rdi        ; primary list
137612c: testq %rdi, %rdi
137612f: je   0x1376165              ; null -> try the secondary
1376131: movq 0x10(%rdi), %rax       ; list->flags
1376135: testl $0x4000000, %eax      ; a "busy" bit
137613a: je   0x13761a5              ; not busy -> release path
137613c: movq $0x0, 0x47c07d0        ; clear the global
137614d: lock xaddl %eax, 0x50(%rdi) ; decrement a refcount
1376156: callq 0xd52f40              ; release
```

So the list is reference-counted at `+0x50`, and bit `0x4000000` of the flags at `+0x10` marks
it as in use. The gate clears the global before releasing, which is why the executor's own
`0x47C07D0` read can be null.

## Threading

The executor checks thread identity at entry, before any dispatch:

```
13764fa: cmpq $0x0, 0x44ac748        ; a global thread record
1376502: je   0x1376522
1376510: callq 0x33c6770             ; scePthreadGetthreadid
1376515: movq 0x44ac748, %rcx
137651c: cmpl 0x44(%rcx), %eax       ; compare against the recorded id
137651f: sete %bl                    ; bl = "we are the recorded thread"
```

And a second check against a different global:

```
1376522: cmpb $0x1, 0x44da124
137652b: callq 0x33c6770             ; scePthreadGetthreadid again
1376530: cmpl 0x44da118, %eax
1376536: sete %r14b                  ; r14b = "we are the second recorded thread"
```

So there are **two** recorded thread identities, and the executor distinguishes them. That is
consistent with a render thread and an RHI thread.

### Bypass modes

The two booleans then select among paths:

```
137653a: cmpb $0x1, 0x47b438a        ; a mode global
1376541: jne  0x137692f              ; mode off -> straight to the walk
1376547: testb %bl, %bl
1376549: je   0x1376651              ; not the first thread -> other path
137654f: testb %r14b, %r14b
1376552: jne  0x137658c              ; is the second thread -> skip the sync
```

So execution can take different paths depending on which recorded thread is running and on the
mode global. The walk at `0x137692F` is reached either directly or after the sync block.

**This is a static guarantee about the gate, not an observation of which thread actually runs
it.** No runtime thread identity was captured for this function.

## Command lifetime

### The executor does not destroy commands

After the walk:

```
137695e: leaq 0x30(%r15), %rdi       ; the allocator
1376962: xorl %esi, %esi
1376964: movb $0x0, 0x10(%r15)       ; bExecuting = false
1376969: callq 0x0E3F2D0             ; reset the allocator
137696e: movl $0x0, 0x14(%r15)       ; NumCommands = 0
1376976: movq $0x0, (%r15)           ; head = null
137697d: movq %r15, 0x8(%r15)        ; tail slot = &head
```

There is **no** `vtable[1]` call anywhere in this sequence. The allocator is reset wholesale and
the list is emptied. So:

- Command destructors do **not** run at execution time.
- Reference-counted resource fields in the payload are **not** released by the executor.
- The payload memory is reclaimed by the allocator reset, not by per-object destruction.

That means a command payload is valid only between append and the allocator reset. A capture that
stores a borrowed pointer into a payload will dangle after the next execution.

### `0x14810` is a different object

A7 classified `0x14810` as RHI command teardown. It is not.

```
1481e: leaq 0x3e88270(%rip), %rax    ; a vtable
14828: addq $0x10, %rax
1482c: movq %rax, (%r15)              ; object->vptr = 0x3E88280
1482f: movl 0x10(%r15), %r14d        ; count at +0x10
14838: movq 0x8(%r15), %rbx          ; array at +0x08
14840: movq (%rbx), %rdi             ; element
14848: movq (%rdi), %rax
1484b: callq *0x8(%rax)              ; element->vtable[1]
```

It installs vtable `0x3E88280` at `+0x00`, walks an array at `+0x08` with a count at `+0x10`,
and calls `vtable[1]` per element. The command list has head at `+0x00`, tail slot at `+0x08`,
`bExecuting` at `+0x10` and `NumCommands` at `+0x14`. Different layout, different vtable,
different lifetime.

So `0x14810` is the destructor of some other container type. It is not RHI command teardown, and
A7's attribution is withdrawn.

## Recursion and re-entrancy

`bExecuting` at `+0x10` is set to 1 before the walk and 0 after. That is a re-entrancy guard, so
the executor cannot recurse into itself on the same list. Whether a command's `Execute` can
append to the same list is not established: the append path does not check `bExecuting` in the
code examined.

## Relationship to GNM submission

Not established. The executor's tail calls `0x0E3F2D0` (allocator reset) and then reads a global
at `0x47B3B08` and calls `vtable[0x3C0]` on it, storing the result at `+0x20(%r15)`. That is a
frame or fence handoff, but its identity was not traced to `sceGnmSubmitCommandBuffers`.

## Confirmed versus inferred

Confirmed by bytes:

- The two list globals and their clearing.
- The refcount at `+0x50` and the busy bit `0x4000000`.
- The two `scePthreadGetthreadid` comparisons and the two recorded ids.
- The mode global at `0x47B438A` and the three-way branch.
- The absence of any `vtable[1]` call in the executor's cleanup.
- `0x14810`'s vtable `0x3E88280` and its array/count offsets.

Inferred:

- That the two recorded threads are the render and RHI threads. The comparison is real; the
  roles are inferred from the two-thread structure.
- That the `0x47B3B08` call is a frame handoff.

## Next targets

1. Identify the two recorded thread ids' owners, to name the threads.
2. Trace `0x47B3B08`'s `vtable[0x3C0]` to the submission path.
3. Determine whether `Execute` can append to the same list.

## Reproducible checks

```
python tools/a5/fde.py v100.elf 0x13764D0 0x1376110 0x14810
llvm-objdump -d --start-address=0x13764FA --stop-address=0x1376560 v100.elf
llvm-objdump -d --start-address=0x137695E --stop-address=0x1376980 v100.elf
llvm-objdump -d --start-address=0x1481E --stop-address=0x14850 v100.elf
```
