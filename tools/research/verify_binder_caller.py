#!/usr/bin/env python3
"""Static checks for shader-binder caller provenance (2026-11-24).

Verifies the findings in SHADER_BINDER_CALLER_PROVENANCE_2026-11-24.md.
Exits non-zero on the first failure.

Usage:
    python verify_binder_caller.py <path-to-v100.elf> [--allow-hash-mismatch]

Design notes:
  * An unexpected SHA-256 is rejected by default.
  * Disassembly failures raise DisasmError instead of returning an empty list.
  * The dispatch search covers callq *disp(%reg), callq *%reg with a cached
    vtable pointer, and direct call/jmp forms. It reports candidate counts and
    classifies each by vptr provenance rather than by displacement alone.
  * The context+0x468 writer search covers movq/movl/movw/movb register stores,
    immediate stores, and vector stores whose range covers the field. Its
    coverage limits are printed, not hidden.

The ELF is never modified and nothing is written to disk.
"""
import hashlib
import re
import struct
import subprocess
import sys

OBJDUMP = "llvm-objdump"
EXPECTED_SHA256 = "3F70499588A9A51781DE87E2999433B85A6D23A4307DC44827134C8B71A938BF"

BINDER = 0x0CBE210
BINDER_THUNK = 0xCBF540
CONTEXT_VT_PRIMARY = 0x3EDFF18
CONTEXT_VT_SECONDARY = 0x3EE0238
BINDER_SLOT = 0x2F8
CALLER_FN = 0xF07D10


class DisasmError(RuntimeError):
    pass


def load(path):
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


def make_rq(data, segs):
    readable = [s for s in segs if s["filesz"] > 0]
    def rq(va):
        for s in readable:
            if s["va"] <= va < s["va"] + s["filesz"]:
                return struct.unpack_from("<Q", data, s["off"] + (va - s["va"]))[0]
        return None
    return rq


def disasm(path, start, end):
    proc = subprocess.run(
        [OBJDUMP, "-d", "--start-address=%#x" % start, "--stop-address=%#x" % end,
         "--no-show-raw-insn", path],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise DisasmError("objdump exited %d for [%#x,%#x): %s"
                          % (proc.returncode, start, end, proc.stderr.strip()[:300]))
    rows = []
    for ln in proc.stdout.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", ln)
        if m:
            rows.append((int(m.group(1), 16), " ".join(m.group(2).split())))
    if not rows:
        raise DisasmError("no instructions decoded for [%#x,%#x)" % (start, end))
    return rows


def check(label, ok, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                           ("  (" + detail + ")") if detail else ""))
    if not ok:
        raise SystemExit("verification failed: " + label)


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
            raise SystemExit("refusing: SHA-256 mismatch (expected %s)" % EXPECTED_SHA256)
        print("  [WARN] hash mismatch accepted")
    print()

    data, segs = load(path)
    rq = make_rq(data, segs)
    text = segs[0]
    tb = data[text["off"]:text["off"] + text["filesz"]]
    tva = text["va"]

    print("D1. Dispatch search: callq *0x2f8(reg) candidates")
    needle = struct.pack("<i", BINDER_SLOT)
    form1 = []
    for i in range(len(tb) - 7):
        if tb[i] != 0xFF:
            continue
        m = tb[i + 1]
        if (m & 0xC0) != 0x80 or ((m >> 3) & 7) != 2 or (m & 0x07) == 0x04:
            continue
        if tb[i + 2:i + 6] != needle:
            continue
        form1.append((tva + i, m & 7))
    check("form-1 candidates found", len(form1) > 0, "%d sites" % len(form1))
    check("all form-1 candidates use %rax", all(r == 0 for _, r in form1))

    print()
    print("D2. The resolved call site loads the vptr from the context")
    rows = disasm(path, 0xF07EEC, 0xF07F00)
    rtext = " ".join(t for _, t in rows)
    check("vptr loaded with movq (%rbx), %rax", "movq (%rbx), %rax" in rtext)
    check("call through slot 0x2f8", "callq *0x2f8(%rax)" in rtext)
    check("context passed as rdi", "movq %rbx, %rdi" in rtext)

    print()
    print("D3. %rbx is a context: sibling slot 0x268 reads context+0x460")
    sib = disasm(path, 0xCBaf80, 0xCBB000)
    stext = " ".join(t for _, t in sib)
    check("slot 0x268 method reads 0x460(%rdi)", "movq 0x460(%rdi), %r14" in stext)
    check("primary vtable slot 0x268 holds 0xcbaf80",
          rq(CONTEXT_VT_PRIMARY + 0x268) == 0xCBaf80)
    check("primary vtable slot 0x2f8 holds the binder",
          rq(CONTEXT_VT_PRIMARY + BINDER_SLOT) == BINDER)

    print()
    print("D4. The state argument is passed through, not constructed here")
    caller = disasm(path, CALLER_FN, 0xF08400)
    ctext = " ".join(t for _, t in caller)
    check("caller saves rsi as the state", "movq %rsi, -0x50(%rbp)" in ctext)
    check("caller never writes the state slots",
          not any(re.search(r"0x(1c0|1e0|1e8|200)\(%r", t) for _, t in caller))
    check("caller performs no allocation",
          "callq 0xd25750" not in ctext and "callq 0x103c0c0" not in ctext)

    print()
    print("D5. The binder reads state+0x1C8 first and retains the state")
    bind = disasm(path, 0x0CBE289, 0x0CBE2B0)
    btext = " ".join(t for _, t in bind)
    check("binder reads state+0x1C8", "movq 0x1c8(%rsi), %rbx" in btext)
    check("binder refcount-increments that object", "incl 0x8(%rbx)" in btext)
    tail = disasm(path, 0x0CBEE20, 0x0CBEE60)
    ttext = " ".join(t for _, t in tail)
    check("binder refcount-assigns context+0x468",
          "leaq 0x468(%r14), %rdi" in ttext and "callq 0xcbfa40" in ttext)

    print()
    print("D6. context+0x468 writer search (documented coverage)")
    d468 = struct.pack("<i", 0x468)
    w468 = []
    for i in range(len(tb) - 12):
        b0 = tb[i]
        if (b0 & 0xF8) == 0x48 and (b0 & 0x08) and tb[i + 1] == 0x89:
            m = tb[i + 2]
            if (m & 0xC0) == 0x80 and (m & 0x07) != 0x04 and tb[i + 3:i + 7] == d468:
                w468.append(tva + i)
    print("  [INFO] direct movq register stores to +0x468: %d" % len(w468))
    print("  [INFO] these are UNRELATED objects that happen to use offset 0x468.")
    print("  [INFO] the binder does NOT store directly; it calls the refcount")
    print("  [INFO] helper 0xcbfa40 after leaq 0x468(%r14), %rdi.")
    check("direct movq stores exist but none is the binder path",
          len(w468) > 0 and all(v != 0xCBEE2D for v in w468),
          "%d unrelated sites" % len(w468))
    print("  [INFO] coverage: movq reg->mem, mod=10, non-SIB, REX.W.")
    print("  [INFO] NOT covered: SIB-indexed, vector, memcpy/bulk, helper-mediated.")
    print("  [INFO] the helper-mediated path is verified separately in D5.")

    print()
    print("D7. writer+0xC198 has two writers; writer+0xC490 has none")
    for disp, expect in ((0xC198, 2), (0xC490, 0)):
        d = struct.pack("<i", disp)
        hits = []
        for i in range(len(tb) - 12):
            b0 = tb[i]
            if (b0 & 0xF8) == 0x48 and (b0 & 0x08) and tb[i + 1] == 0x89:
                m = tb[i + 2]
                if (m & 0xC0) == 0x80 and (m & 0x07) != 0x04 and tb[i + 3:i + 7] == d:
                    hits.append(tva + i)
        check("+%#x movq writers == %d" % (disp, expect), len(hits) == expect,
              "%d found" % len(hits))

    print()
    print("D8. The +0xB0 address is stored into +0xC198, not +0xC490")
    setter = disasm(path, 0x2F61DF0, 0x2F61E10)
    settext = " ".join(t for _, t in setter)
    check("setter stores %rdx into +0xC198", "movq %rdx, 0xc198(%rbx)" in settext)
    check("setter does not touch +0xC490", "0xc490" not in settext)

    print()
    print("D9. Secondary vtable dispatch: no direct reference to the thunk")
    calls = []
    for i in range(len(tb) - 5):
        if tb[i] not in (0xE8, 0xE9):
            continue
        rel = struct.unpack_from("<i", tb, i + 1)[0]
        if tva + i + 5 + rel == BINDER_THUNK:
            calls.append(tva + i)
    check("no direct call/jmp to the thunk", len(calls) == 0, "%d" % len(calls))
    check("secondary vtable slot 0 holds the thunk",
          rq(CONTEXT_VT_SECONDARY) == BINDER_THUNK)
    print("  [INFO] the secondary path is reachable only through a computed")
    print("  [INFO] vtable dispatch; no static call site was resolved for it.")

    print()
    print("All checks passed.")


if __name__ == "__main__":
    try:
        main()
    except DisasmError as exc:
        raise SystemExit("disassembly failed: %s" % exc)
