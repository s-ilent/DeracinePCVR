#!/usr/bin/env python3
"""A4 Phase 1: locate every reference to a target VA in a plain ELF.

Finds direct E8/E9 rel32 call sites and 8-byte absolute pointers, so a dispatch
mechanism can be classified as direct, virtual, or stored-pointer.
"""
import struct
import sys

PT_LOAD = 1


def load(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("not an ELF")
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        p_type = struct.unpack_from("<I", data, o)[0]
        p_flags = struct.unpack_from("<I", data, o + 4)[0]
        p_offset = struct.unpack_from("<Q", data, o + 8)[0]
        p_vaddr = struct.unpack_from("<Q", data, o + 16)[0]
        p_filesz = struct.unpack_from("<Q", data, o + 32)[0]
        p_memsz = struct.unpack_from("<Q", data, o + 40)[0]
        segs.append(dict(type=p_type, flags=p_flags, off=p_offset, va=p_vaddr,
                         filesz=p_filesz, memsz=p_memsz))
    return data, segs


def find_calls(data, segs, target):
    """E8/E9 rel32 whose resolved destination is `target`."""
    hits = []
    for s in segs:
        if s["type"] != PT_LOAD or not (s["flags"] & 1):
            continue
        blob = data[s["off"]:s["off"] + s["filesz"]]
        for i in range(len(blob) - 5):
            if blob[i] not in (0xE8, 0xE9):
                continue
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            src_va = s["va"] + i
            if src_va + 5 + rel == target:
                hits.append((src_va, blob[i]))
    return hits


def find_pointers(data, segs, target):
    """8-byte little-endian values equal to `target`, in any PT_LOAD."""
    needle = struct.pack("<Q", target)
    hits = []
    for s in segs:
        if s["type"] != PT_LOAD:
            continue
        blob = data[s["off"]:s["off"] + s["filesz"]]
        start = 0
        while True:
            j = blob.find(needle, start)
            if j < 0:
                break
            hits.append((s["va"] + j, s["flags"]))
            start = j + 1
    return hits


def main():
    path, target_s = sys.argv[1], sys.argv[2]
    target = int(target_s, 16)
    data, segs = load(path)
    print(f"file={path} target={target:#x}")
    print("--- direct E8/E9 call sites ---")
    calls = find_calls(data, segs, target)
    for va, op in calls:
        print(f"  {va:#x}  opcode={op:#04x}")
    if not calls:
        print("  none")
    print("--- 8-byte pointers ---")
    ptrs = find_pointers(data, segs, target)
    for va, flags in ptrs:
        kind = "exec" if flags & 1 else ("write" if flags & 2 else "ro")
        print(f"  {va:#x}  seg_flags={flags} ({kind})")
    if not ptrs:
        print("  none")


if __name__ == "__main__":
    main()
