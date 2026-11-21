# A7 Phase 1: command production, linking and execution order

## Result

The command list is a **singly linked list with a tail-slot pointer**, and the append is a
single canonical sequence. The executor at `0x14B00` is a *different* structure: it walks an
array of command pointers, not the list.

That distinction corrects A6, which described the executor as walking the list.

## The append mechanism

The canonical append, at `0x13D6906` through `0x13D6919`:

```
13d6906: 48 8b 4b 08           movq   0x8(%rbx), %rcx      ; rcx = list->tail_slot
13d690a: 48 89 01              movq   %rax, (%rcx)         ; *tail_slot = new_cmd
13d690d: 48 89 73 08           movq   %rsi, 0x8(%rbx)      ; list->tail_slot = &cmd->Next
13d6911: 48 c7 40 08 00 00 00 00  movq $0x0, 0x8(%rax)    ; cmd->Next = null
13d6919: 48 89 10              movq   %rdx, (%rax)         ; cmd->vptr = 0x3F28028
```

This is exactly `FRHICommandListBase::AllocCommand`:

```cpp
TCmd* Result = (TCmd*)Alloc();
++NumCommands;
*CommandLink = Result;
CommandLink = &Result->Next;
```

The public header's `CommandLink` is the `tail_slot` here. The append is O(1) and the list is
built in emission order.

## The command-list object

| Offset | Field | Evidence |
| --- | --- | --- |
| `+0x00` | head (`Root`) | `movq (%rbx), %rdi` in the teardown walk |
| `+0x08` | tail slot (`CommandLink`) | the append above |
| `+0x14` | command count | `incl 0x14(%rbx)` |
| `+0x30` | bump allocator cursor | `movq 0x30(%rbx), %rax` |
| `+0x38` | bump allocator end | `cmpq 0x38(%rbx), %rcx` |

Commands are bump-allocated from `+0x30` and the cursor advances by the aligned size. When the
request would pass `+0x38`, the allocator calls `0x0E3EF80` to grow.

## The `0x138` versus `0x130` reconciliation

A6 reported the `SetRenderTargets` command as `0x130` bytes and the allocator request as
`0x138`. Both are correct and they measure different things.

```
13d697a: 48 8d 88 30 01 00 00  leaq   0x130(%rax), %rcx    ; required end = cursor + 0x130
13d6981: 48 3b 4b 38           cmpq   0x38(%rbx), %rcx     ; fits?
13d6985: 76 30                 jbe    0x13d69b7            ; yes -> use it
13d6987: be 38 01 00 00        movl   $0x138, %esi         ; no -> grow by 0x138
```

The `0x130` is the **payload size** the caller needs. The `0x138` is the **growth request**
passed to the allocator, which is `0x130` plus 8 bytes of slack. The allocator is asked for more
than the minimum so that repeated small allocations do not each trigger a grow.

So the command extent is `0x130`, and `0x138` is an allocator hint, not a field size.

## The executor is an array walk

The executor at `0x14B00` (FDE `[0x14B00, 0x14C00)`) does not walk the linked list:

```
14b22: 41 8b 45 20           movl   0x20(%r13), %eax     ; count
14b26: 85 c0                 testl  %eax, %eax
14b28: 0f 84 8c 00 00 00     je     0x14bba
14b2e: 4d 8d 75 08           leaq   0x8(%r13), %r14      ; &array
14b32: 31 db                 xorl   %ebx, %ebx           ; index = 0
14b40: 49 8b 4d 18           movq   0x18(%r13), %rcx     ; array base
14b46: 48 8b 3c d9           movq   (%rcx,%rbx,8), %rdi  ; cmd = array[index]
14b9f: 48 8b 07              movq   (%rdi), %rax         ; cmd->vptr
14ba2: ff 10                 callq  *(%rax)              ; vtable[0]
14ba8: 48 c7 04 d8 00 00 00 00  movq $0x0, (%rax,%rbx,8)  ; array[index] = null
14bb4: ff c3                 incl   %ebx
14bb6: 39 c3                 cmpl   %eax, %ebx
14bb8: 72 86                 jb     0x14b40
```

So there are two distinct structures:

| Structure | Offset | Walked by | Order |
| --- | --- | --- | --- |
| Linked list | `+0x00` head, `+0x08` tail slot | `0x14840` teardown | emission order |
| Pointer array | `+0x18` base, `+0x20` count | `0x14B00` executor | array index order |

The array is a *dispatch queue*, not the command list. The executor also does per-command
bookkeeping at `+0x48` of the command (a sort key) and a deferred-flush call at `0x16830` when
the pending count exceeds a mask.

## Ordering

**Static control-flow ordering is established. Runtime ordering is not.**

What is established:

- Commands are appended in emission order via the tail-slot pointer.
- The linked list preserves that order exactly.
- The executor walks a separate array by index.

What is not established:

- Whether the array order matches the list order. The array is populated elsewhere and the
  population site was not traced.
- Whether the executor runs on the render thread or the RHI thread. The function has no thread
  assertion visible in the decoded range.
- The meaning of the `+0x48` sort key.

So the honest statement is: the *list* is ordered by construction, and the *executor* consumes a
different structure whose ordering relationship to the list is unproven.

## The teardown walk

The function at `0x14810` walks the linked list and calls `vtable[1]` on each command:

```
14838: 49 8b 5f 08           movq   0x8(%r15), %rbx      ; tail slot
14840: 48 8b 3b              movq   (%rbx), %rdi         ; cmd = *slot
14848: 48 8b 07              movq   (%rdi), %rax         ; cmd->vptr
1484b: ff 50 08              callq  *0x8(%rax)           ; vtable[1] = destructor
1484e: 48 c7 03 00 00 00 00  movq   $0x0, (%rbx)         ; clear the slot
14855: 48 83 c3 08           addq   $0x8, %rbx
14859: 41 ff ce              decl   %r14d
1485c: 75 e2                 jne    0x14840
```

Note this walks *slots* (`+0x8` stride), not `Next` pointers. It is walking the same array the
executor uses, calling the destructor on each. So the array is the command storage and the
linked list is a separate ordering structure over the same objects.

That is a meaningful architectural finding: the same command objects are reachable two ways, and
the destructor walk uses the array while the append uses the list.

## Confirmed versus inferred

Confirmed by bytes:

- The append sequence and its four stores.
- The list offsets `+0x00`, `+0x08`, `+0x14`.
- The bump allocator at `+0x30`/`+0x38`.
- The executor's array walk at `+0x18`/`+0x20`.
- The teardown walk's slot stride and `vtable[1]` call.
- The `0x130` payload versus `0x138` growth request.

Inferred:

- That the list is `FRHICommandListBase` and the tail slot is `CommandLink`. The shape matches
  the public header exactly, but the executable is stripped.
- That the array is a dispatch queue. It is walked by index and cleared after execution, which
  is consistent, but its population site was not traced.

## Next targets

1. Find the array population site. That is what would establish whether array order equals list
   order.
2. Identify the `+0x48` sort key.
3. Determine the thread that runs `0x14B00`.
4. Find the flush/submit that turns accumulated commands into PS4 graphics work.

## Reproducible checks

```
python tools/a5/fde.py v100.elf 0x14B00 0x14810 0x13D67E0
llvm-objdump -d --start-address=0x13D6900 --stop-address=0x13D6920 v100.elf
llvm-objdump -d --start-address=0x14B00 --stop-address=0x14BC0 v100.elf
llvm-objdump -d --start-address=0x14810 --stop-address=0x14870 v100.elf
```
