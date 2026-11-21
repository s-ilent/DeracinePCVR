#!/usr/bin/env python3
"""A4: find candidate deferred-command vtables.

A command object in this binary carries a vtable at +0 whose slot 0 is Execute and
whose slot 1 is a trivial destructor. So a command vtable is two consecutive code
pointers where the second is a near-empty function (push rbp; mov rsp,rbp; pop rbp; ret).
"""
import struct
import sys

PT_LOAD = 1
PT_SCE_RELRO = 0x61000010
FILE_BACKED = (PT_LOAD, PT_SCE_RELRO)
CODE_END = 0x3E86D30

TRIVIAL_DTOR = bytes([0x55, 0x48, 0x89, 0xE5, 0x5D, 0xC3])


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


def main():
    path = sys.argv[1]
    data, segs = load(path)
    hits = []
    for s in segs:
        if s["type"] not in FILE_BACKED:
            continue
        blob = data[s["off"]:s["off"] + s["filesz"]]
        for i in range(0, len(blob) - 16, 8):
            a, b = struct.unpack_from("<QQ", blob, i)
            if not (0 < a < CODE_END and 0 < b < CODE_END):
                continue
            off_b = va_to_off(segs, b)
            if off_b is None:
                continue
            if data[off_b:off_b + 6] != TRIVIAL_DTOR:
                continue
            hits.append((s["va"] + i, a, b))
    print(f"file={path} candidate command vtables={len(hits)}")
    for va, a, b in hits[:40]:
        print(f"  vtable {va:#x}  Execute={a:#x}  Dtor={b:#x}")


if __name__ == "__main__":
    main()
