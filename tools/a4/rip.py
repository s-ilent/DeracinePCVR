#!/usr/bin/env python3
"""A4 Phase 1: find RIP-relative LEA/MOV references to a target VA.

A table base is usually reached with `lea disp32(%rip), %reg` rather than stored
as a literal pointer, so a plain pointer scan misses it.
"""
import struct
import sys

PT_LOAD = 1


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


def main():
    path = sys.argv[1]
    targets = {int(t, 16) for t in sys.argv[2:]}
    data, segs = load(path)
    print(f"file={path}")
    for target in sorted(targets):
        print(f"--- RIP-relative references to {target:#x} ---")
        hits = 0
        for s in segs:
            if s["type"] != PT_LOAD or not (s["flags"] & 1):
                continue
            blob = data[s["off"]:s["off"] + s["filesz"]]
            for i in range(len(blob) - 7):
                # 48 8d 05/0d/15/1d/25/2d/35/3d = lea disp32(%rip), %r64
                if blob[i] != 0x48 or blob[i + 1] != 0x8D:
                    continue
                modrm = blob[i + 2]
                if (modrm & 0xC7) != 0x05:
                    continue
                disp = struct.unpack_from("<i", blob, i + 3)[0]
                src = s["va"] + i
                if src + 7 + disp == target:
                    print(f"  {src:#x}  lea {disp:+#x}(%rip)")
                    hits += 1
        if not hits:
            print("  none")


if __name__ == "__main__":
    main()
