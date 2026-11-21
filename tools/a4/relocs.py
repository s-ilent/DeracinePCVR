#!/usr/bin/env python3
"""A4 Phase 1: inspect relative relocations and the slots they populate.

A R_X86_64_RELATIVE relocation stores its target in the relocation entry's
addend, not as a literal pointer in the data. The populated slots live in the
RELRO segment (PT_SCE_RELRO), which is file-backed but not executable.
"""
import struct
import sys

PT_LOAD = 1
PT_DYNAMIC = 2
PT_SCE_RELRO = 0x61000010
FILE_BACKED = (PT_LOAD, PT_SCE_RELRO)
R_X86_64_RELATIVE = 8


def load(path):
    data = open(path, "rb").read()
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        segs.append(dict(
            type=struct.unpack_from("<I", data, o)[0],
            flags=struct.unpack_from("<I", data, o + 4)[0],
            off=struct.unpack_from("<Q", data, o + 8)[0],
            va=struct.unpack_from("<Q", data, o + 16)[0],
            filesz=struct.unpack_from("<Q", data, o + 32)[0],
            memsz=struct.unpack_from("<Q", data, o + 40)[0],
        ))
    return data, segs


def va_to_off(segs, va):
    for s in segs:
        if s["type"] in FILE_BACKED and s["va"] <= va < s["va"] + s["filesz"]:
            return s["off"] + (va - s["va"])
    return None


def dynamic_entries(data, segs):
    out = {}
    for s in segs:
        if s["type"] != PT_DYNAMIC:
            continue
        for i in range(s["filesz"] // 16):
            tag, val = struct.unpack_from("<qQ", data, s["off"] + i * 16)
            if tag == 0:
                break
            out.setdefault(tag, val)
    return out


def relocations(data, segs):
    """Yield (slot_va, type, addend) for every RELA entry."""
    dyn = dynamic_entries(data, segs)
    tables = []
    for addr_tag, size_tag in ((0x6100002f, 0x61000031), (7, 8), (0x61000029, 0x6100002d)):
        if addr_tag in dyn and size_tag in dyn:
            tables.append((dyn[addr_tag], dyn[size_tag]))
    for addr, size in tables:
        off = va_to_off(segs, addr)
        if off is None:
            continue
        for i in range(size // 24):
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", data, off + i * 24)
            yield r_offset, r_info & 0xFFFFFFFF, r_addend


def main():
    path = sys.argv[1]
    mode = sys.argv[2]
    data, segs = load(path)

    if mode == "target":
        targets = {int(t, 16) for t in sys.argv[3:]}
        hits = 0
        for slot, rtype, addend in relocations(data, segs):
            if rtype == R_X86_64_RELATIVE and addend in targets:
                print(f"  SLOT {slot:#x} -> {addend:#x} (RELATIVE)")
                hits += 1
        if not hits:
            print("  no RELATIVE relocation targets the requested addresses")
    elif mode == "slot":
        wanted = {int(s, 16) for s in sys.argv[3:]}
        found = {}
        for slot, rtype, addend in relocations(data, segs):
            if slot in wanted:
                found[slot] = (rtype, addend)
        for slot in sorted(wanted):
            print(f"slot {slot:#x}:")
            if slot in found:
                rtype, addend = found[slot]
                print(f"  relocation type={rtype} addend={addend:#x}")
            else:
                print("  no relocation at this slot")
            off = va_to_off(segs, slot)
            if off is not None:
                print(f"  raw 8 bytes = {struct.unpack_from('<Q', data, off)[0]:#x}")
            else:
                print("  slot not file-backed")
    else:
        raise SystemExit("mode must be target or slot")


if __name__ == "__main__":
    main()
