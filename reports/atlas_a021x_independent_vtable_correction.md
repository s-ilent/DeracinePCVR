# Independent A-021x — command-vtable versus graphics-context-vtable correction

**Read-only original-target investigation.** Déraciné 1.00 PS4 SELF: 81,506,767 bytes; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Addresses below are inner-ELF VAs. For code, original SELF offset = VA + `0x1FD30`; for RELRO, VA + `0x21240`. No original game bytes are distributed and there is no runtime-reachability claim.

The independent Atlas's A-020 interpreted `0x03EDFCA0` as a graphics-context-family vtable. A later scheduled Atlas A-021 also correctly identified this as a deferred node; this report adds **a second sibling node and the separate constructed graphics-context vtable**.

| Address | Verified ownership | Evidence |
| --- | --- | --- |
| `0x03EDFCA0` | Deferred resource-command vtable, Execute `0x0CACDA0` | Producer `0x197588A` constructs and appends a `0x30`-byte linked node; `+0x08` is a trivial virtual method, with zero padding at `+0x10/+0x18` |
| `0x03EDFCC0` | Sibling resource-command vtable, Execute `0x0CACF80` | Independent producer `0x19794A9`, identical `0x30`-byte command header/linked append and reference increments |
| `0x03EDFF18` | Graphics-context vtable address point | Two independent object constructors install this vptr, at `0x0CB2795` and `0x0CE2E8F`, together with secondary vptr `0x03EE0238` |

The known shader-recording method `0x0CB5130` resides at **`0x03EDFF18+0xC8`**, rather than being `0x03EDFCA0+0x340` as A6 originally labeled. The older arithmetic happened to point into the correct memory location while crossing unrelated command vtables. The method address is unaffected, but its vtable owner and slot offset change.

Both commands are linked-list nodes and each Execute reads `[command_list+0x20]` as its **context**; this was previously misread as a context accessed through the node's own vtable. Producer resource references are incremented at `0x19758C8` and `0x19794EE`. Execute performs reference releases itself (e.g. `0x0CACE31..0x0CACE61` and `0x0CAD1A6..0x0CAD1D4`). The generic executor not calling virtual destructors does **not** imply that command-level resource releases never occur.

**Confidence:** original-byte verified for producers, vtable pointers, contexts, and refcount operations; source-level class names, runtime reachability, and shader stage/resource category are not established.

**See:** sibling follow-up `reports/atlas_a022x_shader_user_data_bindings.md` for actual shader-user-data register behavior. Independent original-SELF check: `tools/atlas/verify_independent_a022x.py`, with this vtable relation plus the A-022x register path.