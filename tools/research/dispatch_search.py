#!/usr/bin/env python3
"""Search for indirect dispatch sites that could invoke the shader-state binder.

Covers the call forms the binder can be reached through:
  * callq *disp(%reg)   - direct memory-operand virtual call
  * movq disp(%reg),%rX ; callq *%rX   - cached function pointer
  * leaq disp(%reg),%rX ; callq *%rX   - address-of-then-call
  * callq *%rX where %rX was loaded from a vtable slot

For each candidate it records the vptr origin when it can be resolved, and
classifies the site as primary-context, secondary-context, or unresolved.

Usage:
    python dispatch_search.py <path-to-v100.elf> [--allow-hash-mismatch]
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
BINDER_PRIMARY_SLOT = 0x2F8


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


def fde_starts(data, segs):
    PT_GNU_EH_FRAME = 0x6474E550
    hdr = next((s for s in segs if s["type"] == PT_GNU_EH_FRAME), None)
    if hdr is None:
        raise DisasmError("no PT_GNU_EH_FRAME segment")
    base_off, base_va = hdr["off"], hdr["va"]

    def read_enc(pos, enc, base):
        fmt = enc & 0x0F
        app = enc & 0x70
        if fmt == 0x03:
            raw = struct.unpack_from("<I", data, pos)[0]; pos += 4
        elif fmt == 0x0B:
            raw = struct.unpack_from("<i", data, pos)[0]; pos += 4
        elif fmt == 0x04:
            raw = struct.unpack_from("<Q", data, pos)[0]; pos += 8
        elif fmt == 0x0C:
            raw = struct.unpack_from("<q", data, pos)[0]; pos += 8
        else:
            raise DisasmError("unsupported pointer encoding %#x" % enc)
        if app == 0x10:
            raw += base + pos
        elif app == 0x30:
            raw += base
        return raw, pos

    pos = base_off + 4
    _, pos = read_enc(pos, data[base_off + 1], base_va)
    cnt, pos = read_enc(pos, data[base_off + 2], base_va)
    starts = []
    for _ in range(cnt):
        loc, pos = read_enc(pos, data[base_off + 3], base_va)
        _, pos = read_enc(pos, data[base_off + 3], base_va)
        starts.append(loc)
    starts.sort()
    return starts


def disasm(path, start, end):
    proc = subprocess.run(
        [OBJDUMP, "-d", "--start-address=%#x" % start, "--stop-address=%#x" % end,
         "--no-show-raw-insn", path],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise DisasmError("objdump failed [%#x,%#x): %s"
                          % (start, end, proc.stderr.strip()[:200]))
    rows = []
    for ln in proc.stdout.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", ln)
        if m:
            rows.append((int(m.group(1), 16), " ".join(m.group(2).split())))
    if not rows:
        raise DisasmError("no instructions in [%#x,%#x)" % (start, end))
    return rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    allow = "--allow-hash-mismatch" in sys.argv
    if len(args) != 1:
        raise SystemExit(__doc__)
    path = args[0]

    digest = hashlib.sha256(open(path, "rb").read()).hexdigest().upper()
    print("ELF: " + path)
    print("SHA-256: " + digest)
    if digest != EXPECTED_SHA256 and not allow:
        raise SystemExit("refusing: SHA-256 mismatch (expected %s)" % EXPECTED_SHA256)
    print()

    data, segs = load(path)
    text = segs[0]
    tb = data[text["off"]:text["off"] + text["filesz"]]
    tva = text["va"]
    starts = fde_starts(data, segs)

    import bisect
    def enclosing(va):
        i = bisect.bisect_right(starts, va) - 1
        if i < 0:
            return None
        return starts[i]

    # ---- Form 1: callq *disp(%reg) with disp == 0x2F8 ----
    print("=== Form 1: callq *0x2f8(%reg) ===")
    hits1 = []
    for i in range(len(tb) - 7):
        if tb[i] != 0xFF:
            continue
        m = tb[i + 1]
        if (m & 0xC0) != 0x80 or (m & 0x07) == 0x04:
            continue
        disp = struct.unpack_from("<i", tb, i + 2)[0]
        if disp != BINDER_PRIMARY_SLOT:
            continue
        hits1.append((tva + i, m & 0x07))
    print("  count: %d" % len(hits1))
    for va, reg in hits1[:20]:
        print("    %#x  callq *0x2f8(%%r%d)  in fn %#x" % (va, reg, enclosing(va) or 0))

    # ---- Form 2: any callq *%reg preceded by a load from a vtable slot ----
    print()
    print("=== Form 2: callq *%reg with a vtable-slot load in the preceding window ===")
    # Index all callq *%reg sites
    callreg = []
    for i in range(len(tb) - 3):
        if tb[i] == 0xFF and (tb[i + 1] & 0xC0) == 0xC0 and (tb[i + 1] & 0x38) == 0x10:
            callreg.append((tva + i, tb[i + 1] & 0x07))
    print("  total callq *%%reg sites: %d" % len(callreg))

    # For each, look back for a movq disp(%r),%same_reg
    cand = []
    for va, reg in callreg:
        lo = max(tva, va - 0x60)
        try:
            rows = disasm(path, lo, va + 2)
        except DisasmError:
            continue
        for rva, txt in reversed(rows[:-1]):
            m = re.match(r"movq 0x([0-9a-f]+)\(%r\w+\), %r(\d+)$", txt)
            if m and int(m.group(2)) == reg:
                cand.append((va, reg, int(m.group(1), 16), rva))
                break
    print("  candidates with a matching slot load: %d" % len(cand))
    from collections import Counter
    disp_hist = Counter(d for _, _, d, _ in cand)
    print("  displacement histogram (top 12):")
    for d, n in disp_hist.most_common(12):
        print("    +%#05x : %d" % (d, n))

    # ---- Form 3: any reference to the binder or thunk address ----
    print()
    print("=== Form 3: direct references to the binder / thunk ===")
    for name, tgt in (("binder 0x0CBE210", BINDER), ("thunk 0xCBF540", BINDER_THUNK)):
        calls = []
        for i in range(len(tb) - 5):
            if tb[i] not in (0xE8, 0xE9):
                continue
            rel = struct.unpack_from("<i", tb, i + 1)[0]
            if tva + i + 5 + rel == tgt:
                calls.append(tva + i)
        print("  %s: %d direct call/jmp" % (name, len(calls)))
        for va in calls[:8]:
            print("    %#x in fn %#x" % (va, enclosing(va) or 0))

    # ---- Form 4: vtable slot 0 of the secondary vtable ----
    print()
    print("=== Form 4: callq *(%reg) where reg came from the secondary vtable ===")
    print("  secondary vtable address point: %#x" % CONTEXT_VT_SECONDARY)
    print("  (slot 0 = %#x, the this-adjusting thunk)" % BINDER_THUNK)

    print()
    print("=== Summary ===")
    print("  Form 1 (callq *0x2f8(reg)): %d sites" % len(hits1))
    print("  Form 2 (cached vtable pointer): %d candidates" % len(cand))
    print("  Form 3 (direct refs): see above")


if __name__ == "__main__":
    try:
        main()
    except DisasmError as exc:
        raise SystemExit("disassembly failed: %s" % exc)
