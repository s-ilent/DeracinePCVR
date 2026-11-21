#!/usr/bin/env python3
"""A5: verify A5 claims against the original PS4 SELF, without unwrapping it.

Reads the SELF container directly, maps the inner ELF program headers, and checks
selected bytes, pointers and instruction sequences. Standard library only.

Usage:
    python verify_self.py <eboot.bin> [--json]

Exit code 0 means every check passed.
"""
import json
import struct
import sys

SELF_MAGIC = 0x1D3D154F
PT_LOAD = 1
PT_SCE_RELRO = 0x61000010
FILE_BACKED = (PT_LOAD, PT_SCE_RELRO)
CODE_END = 0x3E86D30


class Self:
    """Minimal SELF reader: container segments plus the inner ELF program headers."""

    def __init__(self, path):
        self.data = open(path, "rb").read()
        if struct.unpack_from("<I", self.data, 0)[0] != SELF_MAGIC:
            raise SystemExit("not a SELF container")
        seg_count = struct.unpack_from("<H", self.data, 0x18)[0]
        self.self_segments = []
        for i in range(seg_count):
            o = 0x20 + i * 0x20
            self.self_segments.append(dict(
                flags=struct.unpack_from("<Q", self.data, o)[0],
                offset=struct.unpack_from("<Q", self.data, o + 8)[0],
                file_size=struct.unpack_from("<Q", self.data, o + 16)[0],
                mem_size=struct.unpack_from("<Q", self.data, o + 24)[0],
            ))
        self.elf_off = 0x20 + seg_count * 0x20
        eh = self.elf_off
        if self.data[eh:eh + 4] != b"\x7fELF":
            raise SystemExit("inner ELF magic missing")
        phoff = struct.unpack_from("<Q", self.data, eh + 0x20)[0]
        phentsize = struct.unpack_from("<H", self.data, eh + 0x36)[0]
        phnum = struct.unpack_from("<H", self.data, eh + 0x38)[0]
        self.phdrs = []
        for i in range(phnum):
            o = eh + phoff + i * phentsize
            self.phdrs.append(dict(
                index=i,
                type=struct.unpack_from("<I", self.data, o)[0],
                flags=struct.unpack_from("<I", self.data, o + 4)[0],
                offset=struct.unpack_from("<Q", self.data, o + 8)[0],
                vaddr=struct.unpack_from("<Q", self.data, o + 16)[0],
                file_size=struct.unpack_from("<Q", self.data, o + 32)[0],
                mem_size=struct.unpack_from("<Q", self.data, o + 40)[0],
            ))

    def container_offset(self, ph):
        """Where a program header's bytes live in the SELF container."""
        for seg in self.self_segments:
            blocked = (seg["flags"] & 0x800) != 0
            seg_id = (seg["flags"] >> 20) & 0xFFF
            if blocked and seg_id == ph["index"]:
                return seg["offset"]
        return None

    def read_va(self, va, length):
        for ph in self.phdrs:
            if ph["type"] not in FILE_BACKED:
                continue
            if ph["vaddr"] <= va and va + length <= ph["vaddr"] + ph["file_size"]:
                base = self.container_offset(ph)
                if base is None:
                    continue
                start = base + (va - ph["vaddr"])
                return self.data[start:start + length]
        return None

    def u64(self, va):
        raw = self.read_va(va, 8)
        return None if raw is None else struct.unpack("<Q", raw)[0]

    def u32(self, va):
        raw = self.read_va(va, 4)
        return None if raw is None else struct.unpack("<I", raw)[0]


CHECKS = []


def check(name, condition, detail=""):
    CHECKS.append(dict(name=name, passed=bool(condition), detail=detail))


def main():
    path = sys.argv[1]
    as_json = "--json" in sys.argv
    self_ = Self(path)

    # --- command vtables: slot 0 is Execute, slot 1 is a trivial destructor ---
    vtables = {
        "SetViewport": 0x03F26A08,
        "SetRenderTargets": 0x03F26A28,
        "SetStreamSource": 0x03F26A48,
        "DrawPrimitive": 0x03F26A68,
        "DrawIndexedPrimitive": 0x03F26A88,
    }
    expected_execute = {
        "SetViewport": 0x13A6BB0,
        "SetRenderTargets": 0x13A6A30,
        "SetStreamSource": 0x13A6930,
        "DrawPrimitive": 0x13A6900,
        "DrawIndexedPrimitive": 0x13A68B0,
    }
    trivial_dtor = bytes([0x55, 0x48, 0x89, 0xE5, 0x5D, 0xC3])

    for name, vt in vtables.items():
        execute = self_.u64(vt)
        dtor = self_.u64(vt + 8)
        check(f"{name}: vtable slot 0 is Execute",
              execute == expected_execute[name],
              f"got {execute:#x}" if execute is not None else "unreadable")
        dtor_bytes = self_.read_va(dtor, 6) if dtor else None
        check(f"{name}: vtable slot 1 is a trivial destructor",
              dtor_bytes == trivial_dtor,
              f"got {dtor_bytes.hex()}" if dtor_bytes else "unreadable")

    # --- the draw wrapper forwards to the PS4 RHI method ---
    wrapper = self_.read_va(0x13A68B0, 0x40)
    check("draw wrapper: E8 rel32 to 0x0CBB9C0 at 0x13A68E3",
          wrapper is not None and wrapper[0x33] == 0xE8 and
          struct.unpack_from("<i", wrapper, 0x34)[0] + 0x13A68E8 == 0x0CBB9C0,
          "call site bytes")

    # --- SetRenderTargets payload offsets ---
    rt_execute = self_.read_va(0x13A6A30, 0x40)
    check("SetRenderTargets: reads count at +0x10",
          rt_execute is not None and b"\x8b\x77\x10" in rt_execute,
          "movl 0x10(%rdi),%esi")
    check("SetRenderTargets: reads UAV count at +0xE8",
          rt_execute is not None and b"\x44\x8b\x87\xe8\x00\x00\x00" in rt_execute,
          "movl 0xe8(%rdi),%r8d")
    check("SetRenderTargets: takes &RT[0] at +0x18",
          rt_execute is not None and b"\x48\x8d\x57\x18" in rt_execute,
          "leaq 0x18(%rdi),%rdx")
    check("SetRenderTargets: takes &depth at +0xD8",
          rt_execute is not None and b"\x48\x8d\x8f\xd8\x00\x00\x00" in rt_execute,
          "leaq 0xd8(%rdi),%rcx")
    check("SetRenderTargets: takes &UAV[0] at +0xF0",
          rt_execute is not None and b"\x4c\x8d\x8f\xf0\x00\x00\x00" in rt_execute,
          "leaq 0xf0(%rdi),%r9")

    # --- SetViewport payload offsets ---
    vp_execute = self_.read_va(0x13A6BB0, 0x30)
    check("SetViewport: MinZ is a float at +0x18",
          vp_execute is not None and b"\xc5\xfa\x10\x47\x18" in vp_execute,
          "vmovss 0x18(%rdi),%xmm0")
    check("SetViewport: MaxZ is a float at +0x24",
          vp_execute is not None and b"\xc5\xfa\x10\x4f\x24" in vp_execute,
          "vmovss 0x24(%rdi),%xmm1")
    check("SetViewport: tail-calls the receiver",
          vp_execute is not None and vp_execute[0x23] == 0xE9,
          "jmp rel32 at +0x23")

    # --- SetStreamSource: the stride division ---
    ss_execute = self_.read_va(0x13A6930, 0xF0)
    check("SetStreamSource: -0x68 buffer adjustment",
          ss_execute is not None and b"\x48\x83\xc3\x98" in ss_execute,
          "addq $-0x68,%rbx")
    check("SetStreamSource: divides size by stride",
          ss_execute is not None and b"\xf7\xf1" in ss_execute,
          "divl %ecx")
    check("SetStreamSource: reads stride from the per-stream table",
          ss_execute is not None and b"\x43\x0f\xb7\x8c\x7d\xa8\x00\x00\x00" in ss_execute,
          "movzwl 0xa8(%r13,%r15,2),%ecx")

    # --- the vptr store that establishes the command header ---
    ctor = self_.read_va(0x141015C, 0x50)
    check("command header: vptr stored at +0x00",
          ctor is not None and b"\x48\x89\x30" in ctor,
          "movq %rsi,(%rax)")
    check("command header: Next cleared at +0x08",
          ctor is not None and b"\x48\xc7\x40\x08\x00\x00\x00\x00" in ctor,
          "movq $0x0,0x8(%rax)")

    # --- the execute loop dispatches through vtable[0] ---
    loop = self_.read_va(0x14B97, 0x10)
    check("execute loop: calls vtable[0]",
          loop is not None and loop[0x08] == 0x48 and loop[0x0B] == 0xFF and loop[0x0C] == 0x10,
          "movq (%rdi),%rax; callq *(%rax)")

    passed = sum(1 for c in CHECKS if c["passed"])
    total = len(CHECKS)

    if as_json:
        print(json.dumps(dict(file=path, passed=passed, total=total, checks=CHECKS), indent=2))
    else:
        print(f"file={path}")
        for c in CHECKS:
            mark = "PASS" if c["passed"] else "FAIL"
            print(f"  [{mark}] {c['name']}" + (f"  ({c['detail']})" if not c["passed"] else ""))
        print(f"{passed}/{total} checks passed")

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
