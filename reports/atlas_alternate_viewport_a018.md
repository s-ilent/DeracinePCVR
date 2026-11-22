# A-018 — Alternate viewport producer and mode-2 command chain

**Verified original-target static finding, not runtime reachability.** Déraciné 1.00 SELF SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, rehashed from private Library raw bytes in this run. All addresses below are **inner ELF VAs**. For the observed executable PT_LOAD#0, original SELF offset = ELF VA + `0x1FD30`; these are neither loaded guest addresses nor GPU VAs.

## Independent instruction-boundary evidence

A-017 established that caller `0x14002A0` branches on owner `+0xD8`, invoking `0x1404710` on the nonzero path versus `0x1400A40` on zero. This run independently decoded the **previously unexplored alternate callee** `0x1404710` (SELF offset `0x1424440`), with GNU objdump x86-64 from the original bytes.

- `0x140474E`: reads arena from `[rbx+0x28]` and calls shared helper `0x13D67E0` with three zero registers; this matches the existing command-list arena pattern but does not name the helper.
- `0x1404757–0x1404777`: reads two packed 64-bit coordinates from incoming `RCX` (`r15`) at `+0x00/+0x08`, splits into four 32-bit lanes, and stores them to owner `[rbx+0x30,+0x34,+0x38,+0x3C]`. Thus **both producers use the same four-field owner cache**, rather than distinct coordinate formats.
- `0x1404783–0x1404803`: checks arena `+0x30/+0x38` capacity, optionally calls growth routine `0x0E3EF80`, links a 0x28-byte command into list tail `+0x08`, increments count `+0x14`, and writes **vptr `0x03F26A08`**, the independently established deferred viewport Execute vtable. Payload `+0x10,+0x14,+0x1C,+0x20` receives four coordinates; `+0x18=0` and `+0x24=0x3F800000` encode depth 0.0 and 1.0. This is a **shared viewport command type**, not a new platform RHI viewport variant.
- `0x140480E–0x1404815`: **second discriminator**, `cmp dword [owner+0xD8],2; jne 0x1404C37`. Therefore the nonzero branch chosen upstream is further divided: **mode 2** follows additional setup, while other nonzero modes jump to a different block. The semantic meaning of mode 2 is not established.
- On mode 2, `0x1404879–0x1404882` performs a virtual call through global `0x047B3B08` object vptr slot `+0xD8` with a stack output location and global `0x047D8BA0` input; the returned pointer is used at `0x1404886`. No API name or resource class is inferred.
- `0x14048D7–0x1404945`: arena allocation of a **second, 0x30-byte linked command** with vptr `0x03EDF C00` (remove space: `0x03EDFC00`), payload `+0x10=1`, `+0x18` holding an arena-stored pointer, `+0x20=2`, `+0x28` holding the virtual-call result. This is distinct from the viewport command; its Execute and lifetime remain unclassified.
- `0x1404949–0x14049C1`: appends a **third, 0x18-byte command** to a *global* arena/list with cursor `0x047B3D18`, limit `0x047B3D20`, count `0x047B3CFC`, tail `0x047B3CF0`, vptr `0x03EDFC80`, and payload `+0x10` equal to the pointer returned from the virtual call. This is **not** the same list as the owner `[rbx+0x28]` arena. The subsequent `r9d` 0..4 jump table at `0x14049C5–0x14049E1` branches into further work; it is not interpreted here.

## Conclusion and limitations

**High confidence:** alternate `0x1404710` begins by constructing the *same* deferred viewport command as `0x1400A40`, but owner mode `+0xD8==2` adds a resource-oriented command to the local list and a pointer-bearing command to a separate global list. This exposes a **multi-list ownership boundary above the RHI writer**. It may represent a specialized viewport/surface handoff or resource registration; there is no evidence yet that it is stereo, eye-specific, or executed during a captured frame.

**Unresolved:** the target vtables `0x03EDFC00` and `0x03EDFC80`, virtual slot `+0xD8`, mode `+0xD8` producer, and non-2 branch `0x1404C37`. The first two table addresses are *ELF data addresses*, not proof of a particular C++ type. This report does not assert their relocated method pointers.

**Reproduce:** `sha256sum eboot-raw-access.bin`; extract only `[0x1424440,0x1424B90)` privately from the original SELF, then `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0x1404710`. Confirm instruction boundaries and all branch/field operations above. Do not publish extracted bytes.

**Next:** resolve relocations and Execute functions for `0x03EDFC00/0x03EDFC80`, and decode the `0x1404C37` alternate-mode branch before interpreting the multi-list handoff.
