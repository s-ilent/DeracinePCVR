#!/usr/bin/env python3
"""Focused static checks for the shader-resource lifecycle correction.

Verifies, against a Deracine inner ELF, the claims the correction report
depends on. Exits non-zero on the first failed assertion.

Usage:
    python verify_shader_resource_correction.py <path-to-v100.elf>

The ELF is never modified and nothing is written to disk.
"""
import hashlib
import re
import struct
import subprocess
import sys

OBJDUMP = "llvm-objdump"

EXPECTED_SHA256 = "3F70499588A9A51781DE87E2999433B85A6D23A4307DC44827134C8B71A938BF"

FACTORY_A = 0x135B4F0
FACTORY_A_END = 0x135B740
CTOR = 0x135B9B0
CTOR_END = 0x135BBB0
BINDER = 0x0CBE210
BINDER_END = 0x0CBEE90
RECEIVER = 0x0CCC2F0
RECEIVER_END = 0x0CCD480

FACTORY_ALLOC = 0x1D8
INSTANCE_SIZE_FIELD = 0x1F8


def disasm(path, start, end):
    out = subprocess.run(
        [OBJDUMP, "-d", "--start-address=%#x" % start, "--stop-address=%#x" % end,
         "--no-show-raw-insn", path],
        capture_output=True, text=True).stdout
    rows = []
    for ln in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", ln)
        if m:
            rows.append((int(m.group(1), 16), m.group(2).strip()))
    return rows


def max_byte_touched(rows, base):
    """Highest byte offset written through base, accounting for store width."""
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


def check(label, ok, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                           ("  (" + detail + ")") if detail else ""))
    if not ok:
        raise SystemExit("verification failed: " + label)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]

    digest = hashlib.sha256(open(path, "rb").read()).hexdigest().upper()
    print("ELF: " + path)
    print("SHA-256: " + digest)
    if digest != EXPECTED_SHA256:
        print("  [WARN] hash differs from the report target " + EXPECTED_SHA256)
        print("         address checks below may not apply to this binary")
    print()

    print("C1. The factory object is 472 bytes and fits its own allocation")
    rows = disasm(path, FACTORY_A, FACTORY_A_END)
    high, where = max_byte_touched(rows, "rbx")
    check("factory writes reach +%#x (%d bytes)" % (high, high + 1),
          high + 1 <= FACTORY_ALLOC,
          "alloc 0x%x, last write %#x: %s" % (FACTORY_ALLOC, where[0], where[1]))

    print()
    print("C2. The factory materialises the instance size 0x1F8 at +0x40")
    found = None
    for va, txt in rows:
        if "movabsq" in txt and "%rdx" in txt:
            for tok in txt.replace(",", " ").split():
                if tok.startswith("$0x"):
                    val = int(tok[1:], 16)
                    if val & 0xFFFFFFFF == INSTANCE_SIZE_FIELD:
                        found = (va, val)
    check("factory materialises 0x1000001f8", found is not None,
          ("%#x" % found[0]) if found else "not found")
    stored = any("movq" in t and "%rdx" in t and "0x40(%rbx)" in t for _, t in rows)
    check("factory stores it at +0x40", stored)

    print()
    print("C3. The constructor needs more than 472 bytes")
    ctor_rows = disasm(path, CTOR, CTOR_END)
    chigh, cwhere = max_byte_touched(ctor_rows, "rax")
    check("constructor writes reach +%#x (%d bytes)" % (chigh, chigh + 1),
          chigh + 1 > FACTORY_ALLOC,
          "last write %#x: %s" % (cwhere[0], cwhere[1]))
    check("constructor fits the 0x%x descriptor" % INSTANCE_SIZE_FIELD,
          chigh + 1 <= INSTANCE_SIZE_FIELD,
          "%d <= %d" % (chigh + 1, INSTANCE_SIZE_FIELD))
    check("the +0x1D8 program-data pointer lies inside the instance",
          0x1D8 + 8 <= INSTANCE_SIZE_FIELD)

    print()
    print("C4. The factory stores the constructor at +0x98 as a data field")
    ctor_ref = any("leaq" in t and "%rdx" in t and "(%rip)" in t for _, t in rows)
    stored_ctor = any("movq" in t and "%rdx" in t and "0x98(%rbx)" in t for _, t in rows)
    check("factory loads a RIP-relative address into %rdx", ctor_ref)
    check("factory stores it at +0x98", stored_ctor)

    print()
    print("C5. The receiver reads all three slots and null-tests them")
    recv = disasm(path, RECEIVER, RECEIVER_END)
    text = " ".join(" ".join(t.split()) for _, t in recv)
    check("receiver reads state+0x1E8", "0x1e8(%rax)" in text)
    check("receiver reads state+0x200", "0x200(%rax)" in text)
    check("receiver reads state+0x1E0", "0x1e0(%rax)" in text)
    check("receiver null-tests the +0x200 slot",
          "movq 0x200(%rax), %r13" in text and "testq %r13, %r13" in text)
    check("receiver null-tests the +0x1E8 slot",
          "movq 0x1e8(%rax), %rax" in text and "testq %rax, %rax" in text)

    print()
    print("C6. Slot to GNM setter mapping")
    mapping = {}
    for i, (va, raw) in enumerate(recv):
        txt = " ".join(raw.split())
        slot = None
        for cand in ("0x1e0", "0x1e8", "0x200"):
            if "movq " + cand + "(%rax)" in txt:
                slot = int(cand, 16)
        if slot is None:
            continue
        for j in range(i, min(i + 40, len(recv))):
            t = " ".join(recv[j][1].split())
            if "0x33c8340" in t:
                mapping.setdefault(slot, "PS")
                break
            if "0x33c81b0" in t:
                mapping.setdefault(slot, "VS")
                break
    check("state+0x1E8 feeds sceGnmSetPsShader350", mapping.get(0x1E8) == "PS",
          "got " + str(mapping.get(0x1E8)))
    check("state+0x200 feeds sceGnmSetVsShader", mapping.get(0x200) == "VS",
          "got " + str(mapping.get(0x200)))
    check("state+0x1E0 feeds sceGnmSetVsShader", mapping.get(0x1E0) == "VS",
          "got " + str(mapping.get(0x1E0)))

    print()
    print("C7. The binder takes (context, new_state) and refcount-assigns context+0x468")
    bind = disasm(path, BINDER, BINDER_END)
    btext = " ".join(" ".join(t.split()) for _, t in bind)
    check("binder saves %rdi as the context", "movq %rdi, %r14" in btext)
    check("binder saves %rsi as the new state", "movq %rsi, -0x50(%rbp)" in btext)
    check("binder refcount-assigns context+0x468", "leaq 0x468(%r14), %rdi" in btext)
    check("binder reads all three slots",
          all(s in btext for s in ("0x1e0(%r15)", "0x1e8(%r15)", "0x200(%r15)")))

    print()
    print("C8. The descriptor is instantiated by calling its +0x98 data field")
    inst = disasm(path, 0xF10910, 0xF10DE0)
    itext = " ".join(" ".join(t.split()) for _, t in inst)
    check("instantiation helper tests the descriptor flag 0x10000080",
          "0x10000080" in itext)
    check("instantiation helper loads the +0x98 data field",
          "movq 0x98(%r12), %r15" in itext)
    check("instantiation helper calls it indirectly", "callq *%r15" in itext)
    check("instantiation helper allocates via 0x1038500", "callq 0x1038500" in itext)

    print()
    print("All checks passed.")


if __name__ == "__main__":
    main()
