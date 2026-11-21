#!/usr/bin/env python3
"""A4: map import stub addresses to symbol names.

The PLT/JUMP_SLOT relocations give (stub address -> symbol index). The dynamic
symbol table gives (symbol index -> name). Together they name each import stub,
which is what makes a call site like `callq 0x33c8340` readable.
"""
import struct
import sys

PT_LOAD = 1
PT_DYNAMIC = 2
PT_SCE_RELRO = 0x61000010
FILE_BACKED = (PT_LOAD, PT_SCE_RELRO)

DT = {
    0x61000029: "JMPREL",
    0x6100002d: "PLTRELSZ",
    0x61000035: "STRTAB",
    0x61000037: "STRSZ",
    0x61000039: "SYMTAB",
    0x6100003f: "SYMTABSZ",
}
R_X86_64_JUMP_SLOT = 7


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


def main():
    path = sys.argv[1]
    wanted = {int(a, 16) for a in sys.argv[2:]} if len(sys.argv) > 2 else None
    data, segs = load(path)
    dyn = dynamic_entries(data, segs)

    strtab_off = va_to_off(segs, dyn[0x61000035])
    symtab_off = va_to_off(segs, dyn[0x61000039])
    sym_count = dyn[0x6100003f] // 24
    jmprel_off = va_to_off(segs, dyn[0x61000029])
    plt_count = dyn[0x6100002d] // 24

    def sym_name(idx):
        o = symtab_off + idx * 24
        st_name = struct.unpack_from("<I", data, o)[0]
        end = data.index(b"\x00", strtab_off + st_name)
        return data[strtab_off + st_name:end].decode("ascii", "replace")

    print(f"file={path} symbols={sym_count} plt_entries={plt_count}")
    for i in range(plt_count):
        r_offset, r_info, r_addend = struct.unpack_from("<QQq", data, jmprel_off + i * 24)
        r_type = r_info & 0xFFFFFFFF
        sym_idx = r_info >> 32
        if r_type != R_X86_64_JUMP_SLOT:
            continue
        if wanted is not None and r_offset not in wanted:
            continue
        print(f"  stub {r_offset:#x}  {sym_name(sym_idx)}")


if __name__ == "__main__":
    main()
