#!/usr/bin/env python3
"""A6: find deferred-command Execute functions by their calling convention.

A command Execute has the shape `void Execute(FRHICommandListBase& CmdList)`:
the command object is in %rdi and the command list in %rsi. It loads the context
from 0x20(%rsi) and forwards to a receiver.

This scans for that prologue and reports each candidate with its payload reads.
"""
import struct
import sys

PT_LOAD = 1
PT_SCE_RELRO = 0x61000010
FILE_BACKED = (PT_LOAD, PT_SCE_RELRO)
CODE_END = 0x3E86D30

# movq 0x20(%rsi), %rax / %r13 / %r12 / %rbx / %rdi
CONTEXT_LOADS = [
    bytes([0x48, 0x8B, 0x46, 0x20]),
    bytes([0x4C, 0x8B, 0x6E, 0x20]),
    bytes([0x4C, 0x8B, 0x66, 0x20]),
    bytes([0x48, 0x8B, 0x5E, 0x20]),
    bytes([0x48, 0x8B, 0x7E, 0x20]),
]


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


def main():
    path = sys.argv[1]
    data, segs = load(path)
    text = next(s for s in segs if s["type"] == PT_LOAD and s["va"] == 0)
    blob = data[text["off"]:text["off"] + text["filesz"]]

    hits = []
    for i in range(len(blob) - 0x40):
        # A command Execute starts with the standard frame prologue.
        if blob[i] != 0x55 or blob[i + 1:i + 3] != b"\x48\x89\xe5":
            continue
        window = blob[i:i + 0x40]
        if not any(pat in window for pat in CONTEXT_LOADS):
            continue
        hits.append(text["va"] + i)

    print(f"file={path} candidates={len(hits)}")
    for va in hits:
        off = va
        window = blob[off:off + 0x40]
        # Report the payload offsets read from %rdi in the first 0x40 bytes.
        reads = []
        for j in range(len(window) - 3):
            if window[j] in (0x8B, 0x44, 0x4C, 0x48) and window[j + 1] in (0x47, 0x4F, 0x57, 0x5F, 0x77, 0x7F, 0x87, 0x8F):
                reads.append(window[j:j + 4].hex())
        print(f"  {va:#x}  payload_reads={reads[:4]}")


if __name__ == "__main__":
    main()
