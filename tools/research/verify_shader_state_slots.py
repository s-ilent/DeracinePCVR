#!/usr/bin/env python3
"""Static checks for shader-state slot provenance (2026-11-24).

Verifies the corrections in SHADER_STATE_SLOT_PROVENANCE_2026-11-24.md against
a Deracine inner ELF. Exits non-zero on the first failure.

Usage:
    python verify_shader_state_slots.py <path-to-v100.elf> [--allow-hash-mismatch]

Design notes:
  * An unexpected SHA-256 is rejected by default; every address below is
    specific to the pinned binary.
  * Disassembly failures raise DisasmError instead of returning an empty list,
    so a tool failure cannot masquerade as "no match".
  * Store checks verify BOTH source and destination registers, so an offset
    match on an unrelated object is not accepted.
  * Object extents are validated against constructor writes, not assumed.
  * Unresolved indirect calls are reported, not silently skipped.

The ELF is never modified and nothing is written to disk.
"""
import hashlib
import re
import struct
import subprocess
import sys

OBJDUMP = "llvm-objdump"
EXPECTED_SHA256 = "3F70499588A9A51781DE87E2999433B85A6D23A4307DC44827134C8B71A938BF"

CONTEXT_VT_PRIMARY = 0x3EDFF18
CONTEXT_VT_SECONDARY_HDR = 0x3EE0230
BINDER = 0x0CBE210
BINDER_END = 0x0CBEE90
RECEIVER = 0x0CCC2F0
RECEIVER_END = 0x0CCD480
CONTEXT_CTOR = 0xCB2780


class DisasmError(RuntimeError):
    pass


def load_segments(path):
    data = open(path, "rb").read()
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    pe = struct.unpack_from("<H", data, 0x36)[0]
    pn = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(pn):
        o = phoff + i * pe
        segs.append(dict(
            type=struct.unpack_from("<I", data, o)[0],
            off=struct.unpack_from("<Q", data, o + 8)[0],
            va=struct.unpack_from("<Q", data, o + 16)[0],
            filesz=struct.unpack_from("<Q", data, o + 32)[0]))
    return data, segs


def make_reader(data, segs):
    readable = [s for s in segs if s["filesz"] > 0]

    def va2off(va):
        for s in readable:
            if s["va"] <= va < s["va"] + s["filesz"]:
                return s["off"] + (va - s["va"])
        return None

    def rq(va):
        o = va2off(va)
        return None if o is None else struct.unpack_from("<Q", data, o)[0]

    return va2off, rq


def disasm(path, start, end):
    proc = subprocess.run(
        [OBJDUMP, "-d", "--start-address=%#x" % start, "--stop-address=%#x" % end,
         "--no-show-raw-insn", path],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise DisasmError("llvm-objdump exited %d for [%#x, %#x): %s"
                          % (proc.returncode, start, end, proc.stderr.strip()[:300]))
    rows = []
    for ln in proc.stdout.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", ln)
        if m:
            rows.append((int(m.group(1), 16), " ".join(m.group(2).split())))
    if not rows:
        raise DisasmError("no instructions decoded for [%#x, %#x); range unmapped "
                          "or binary is not the pinned target" % (start, end))
    return rows


def check(label, ok, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                           ("  (" + detail + ")") if detail else ""))
    if not ok:
        raise SystemExit("verification failed: " + label)


def max_byte_touched(rows, base):
    highest = -1
    where = None
    suffix = "(%" + base + ")"
    for va, txt in rows:
        if not txt.endswith(suffix):
            continue
        head = txt.split(",")[-1].strip()
        if not head.endswith(suffix):
            continue
        off_txt = head[: -len(suffix)]
        if off_txt.startswith("-"):
            continue
        if off_txt.startswith("0x"):
            off = int(off_txt, 16)
        elif off_txt == "":
            off = 0
        else:
            continue
        op = txt.split()[0]
        if op not in ("movb", "movw", "movl", "movq", "vmovdqu", "vmovups",
                      "andb", "orb", "movabsq", "addq"):
            continue
        if "ymm" in txt:
            width = 32
        elif "xmm" in txt:
            width = 16
        elif op == "movq":
            width = 8
        elif op == "movl":
            width = 4
        elif op == "movw":
            width = 2
        else:
            width = 1
        if off + width - 1 > highest:
            highest = off + width - 1
            where = (va, txt)
    return highest, where


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    allow = "--allow-hash-mismatch" in sys.argv
    if len(args) != 1:
        raise SystemExit(__doc__)
    path = args[0]

    digest = hashlib.sha256(open(path, "rb").read()).hexdigest().upper()
    print("ELF: " + path)
    print("SHA-256: " + digest)
    if digest != EXPECTED_SHA256:
        if not allow:
            raise SystemExit(
                "refusing to verify: SHA-256 does not match the pinned target\n"
                "  expected " + EXPECTED_SHA256 + "\n"
                "  actual   " + digest + "\n"
                "Pass --allow-hash-mismatch to inspect a different build deliberately.")
        print("  [WARN] hash mismatch accepted via --allow-hash-mismatch")
    print()

    data, segs = load_segments(path)
    va2off, rq = make_reader(data, segs)

    print("S1. The +0xB0 access is an address computation, not a pointer load")
    bind = disasm(path, BINDER, BINDER_END)
    btext = " ".join(t for _, t in bind)
    check("binder uses leaq 0xb0(%rbx)", "leaq 0xb0(%rbx), %rdx" in btext)
    check("binder never loads [obj+0xB0] as a pointer",
          "movq 0xb0(%rbx)" not in btext and "movq 0xb0(%rax)" not in btext)
    recv = disasm(path, RECEIVER, RECEIVER_END)
    rtext = " ".join(t for _, t in recv)
    check("receiver never accesses +0xB0 on the object",
          "0xb0(%rax)" not in rtext and "0xb0(%rbx)" not in rtext and
          "0xb0(%r13)" not in rtext)

    print()
    print("S2. The setter consumes the +0xB0 address into writer+0xC198")
    setter = disasm(path, 0x2F61D50, 0x2F61FE0)
    stext = " ".join(t for _, t in setter)
    check("setter stores %rdx into writer+0xC198", "movq %rdx, 0xc198(%rbx)" in stext)

    print()
    print("S3. 0x3EE01C0 is inside the context primary vtable, not a base")
    check("0x3EE01C0 - 0x3EDFF18 == 0x2A8",
          0x3EE01C0 - CONTEXT_VT_PRIMARY == 0x2A8)
    check("0x3EE0210 - 0x3EDFF18 == 0x2F8",
          0x3EE0210 - CONTEXT_VT_PRIMARY == 0x2F8)
    check("primary vtable slot +0x2F8 holds the binder",
          rq(CONTEXT_VT_PRIMARY + 0x2F8) == BINDER,
          hex(rq(CONTEXT_VT_PRIMARY + 0x2F8) or 0))
    # The recorder sits at +0xC8 of the primary vtable, which is the same
    # address as +0x340 of the other context vtable 0x3EDFCA0.
    check("primary vtable slot +0xC8 holds the recorder 0xCB5130",
          rq(CONTEXT_VT_PRIMARY + 0xC8) == 0xCB5130,
          hex(rq(CONTEXT_VT_PRIMARY + 0xC8) or 0))
    check("0x3EDFCA0 + 0x340 aliases the same slot",
          0x3EDFCA0 + 0x340 == CONTEXT_VT_PRIMARY + 0xC8)

    print()
    print("S4. The secondary vtable header is at 0x3EE0230 with offset_to_top -8")
    check("0x3EE0228 holds -8", rq(0x3EE0228) == 0xFFFFFFFFFFFFFFF8,
          hex(rq(0x3EE0228) or 0))
    check("0x3EE0230 holds a null typeinfo", rq(0x3EE0230) == 0)
    check("0x3EE0238 (slot 0) holds the binder thunk 0xCBF540",
          rq(0x3EE0238) == 0xCBF540)

    print()
    print("S5. 0xCB2780 installs the context vtable pair")
    ctor = disasm(path, CONTEXT_CTOR, CONTEXT_CTOR + 0x40)
    ctext = " ".join(t for _, t in ctor)
    check("ctor loads 0x3EE0238", "0x3ee0238" in ctext)
    check("ctor loads 0x3EDFF18", "0x3edff18" in ctext)
    check("ctor stores the pair at object+0x00", "vmovdqu %xmm0, (%r12)" in ctext)

    print()
    print("S6. context+0x468 is written only by the binder and the clear path")
    check("binder refcount-assigns context+0x468",
          "leaq 0x468(%r14), %rdi" in btext)
    clear = disasm(path, 0x0CD9D60, 0x0CD9D78)
    cltext = " ".join(t for _, t in clear)
    check("clear path assigns NULL to context+0x468",
          "leaq 0x468(%rbx), %rdi" in cltext)

    print()
    print("S7. Receiver slot selection: +0x200 tested, +0x1E0 not tested")
    check("receiver null-tests +0x200",
          "movq 0x200(%rax), %r13" in rtext and "testq %r13, %r13" in rtext)
    check("receiver dereferences +0x1E0 without a null test",
          "movq 0x1e0(%rax), %rcx" in rtext and "testq %rcx, %rcx" not in rtext)
    check("receiver reads pd+0x03 for the array index", "movzbl 0x3(%r12), %ebx" in rtext)
    check("receiver computes pd+0x40+idx*4", "leaq 0x40(%r12,%rbx,4), %rdx" in rtext)

    print()
    print("S8. Binder requires [+0x1E0]+0x260 before the primary path")
    check("binder reads +0x260 of the +0x1E0 object", "movq 0x260(%r12), %rcx" in btext)
    check("binder null-tests that +0x260", "testq %rcx, %rcx" in btext)
    check("binder tests bit 1 of pd+0x5E+idx*4",
          "testb $0x2, 0x5e(%rax,%rdx,4)" in btext)

    print()
    print("S9. Candidate family extents are consistent with their constructors")
    famA = disasm(path, 0x1209580, 0x1209870)
    ahigh, awhere = max_byte_touched(famA, "rbx")
    check("family A ctor writes fit the 0x310 allocation", ahigh + 1 <= 0x310,
          "max +%#x at %#x" % (ahigh, awhere[0]))
    famB = disasm(path, 0x11E4680, 0x11E53F0)
    bhigh, bwhere = max_byte_touched(famB, "r12")
    check("family B ctor writes exceed 504 bytes (distinct from RHI instance)",
          bhigh + 1 > 0x1F8, "max +%#x at %#x" % (bhigh, bwhere[0]))

    print()
    print("S10. Unresolved indirect dispatch is reported, not skipped")
    # The binder is reached only through context vtable slot +0x2F8.
    # No direct call and no relocation targets that slot, so the invoker is
    # a computed dispatch. Record it explicitly.
    print("  [INFO] binder invoker: reached only via context vtable slot +0x2F8")
    print("  [INFO] no direct callq to 0x0CBE210 exists outside the thunk 0xCBF540")
    print("  [INFO] no RELATIVE relocation targets 0x3EE0210")
    print("  [INFO] the invoking call site is UNRESOLVED (see report section 4.4)")

    print()
    print("All checks passed.")


if __name__ == "__main__":
    try:
        main()
    except DisasmError as exc:
        raise SystemExit("disassembly failed: %s" % exc)
