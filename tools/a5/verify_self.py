#!/usr/bin/env python3
"""A5: verify A5 claims against the original PS4 SELF, without unwrapping it.

Reads the SELF container directly, maps the inner ELF program headers, and checks
selected bytes, pointers and instruction sequences. Standard library only.

Usage:
    python verify_self.py <eboot.bin> [--json]

Exit code 0 means every check passed.
"""
import hashlib
import json
import struct
import sys

SELF_MAGIC = 0x1D3D154F
PT_LOAD = 1
PT_SCE_RELRO = 0x61000010
FILE_BACKED = (PT_LOAD, PT_SCE_RELRO)
CODE_END = 0x3E86D30

# Approved images. Addresses are only meaningful for these exact bytes.
APPROVED_IMAGES = {
    "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0": "1.00",
    "8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75": "01.01",
}
APPROVED_SIZE = 81506767


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

    # Identity first: the addresses below are only meaningful for the approved bytes.
    raw = open(path, "rb").read()
    digest = hashlib.sha256(raw).hexdigest()
    size = len(raw)
    check("image: size is the approved 81,506,767 bytes",
          size == APPROVED_SIZE, f"got {size}")
    check("image: SHA-256 is an approved 1.00 or 01.01 build",
          digest in APPROVED_IMAGES, f"got {digest}")
    if digest not in APPROVED_IMAGES or size != APPROVED_SIZE:
        print(f"refusing to interpret {path}: not an approved image", file=sys.stderr)
        print(f"  size={size} sha256={digest}", file=sys.stderr)
        return 2

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

    # --- SetRenderTargets constructor: exact field extents ---
    # The depth view is a two-pointer pair at +0xD8/+0xE0, so it ends at +0xE8 where
    # the UAV count begins. The UAV array runs from +0xF0 to the 0x130 allocation end.
    rt_ctor = self_.read_va(0x13D6A9E, 0x30)
    check("SetRenderTargets ctor: depth view is 16 bytes at +0xD8",
          rt_ctor is not None and b"\x4c\x89\xb8\xd8\x00\x00\x00" in rt_ctor and
          b"\x48\x89\x90\xe0\x00\x00\x00" in rt_ctor,
          "movq %r15,0xd8; movq %rdx,0xe0")
    check("SetRenderTargets ctor: UAV count at +0xE8 does not overlap depth",
          rt_ctor is not None and b"\xc7\x80\xe8\x00\x00\x00\x00\x00\x00\x00" in rt_ctor,
          "movl $0x0,0xe8(%rax)")
    check("SetRenderTargets ctor: allocation is 0x130 bytes",
          self_.read_va(0x13D6987, 5) == b"\xbe\x38\x01\x00\x00",
          "movl $0x138,%esi (0x130 payload + 8 header)")

    # --- SetRenderTargets receiver: UAV array is 8-byte pointers ---
    rt_recv = self_.read_va(0xCC2B20, 0x10)
    check("SetRenderTargets receiver: UAV array indexed as 8-byte entries",
          rt_recv is not None and b"\x4e\x8b\x2c\xf8" in rt_recv,
          "movq (%rax,%r15,8),%r13")
    check("SetRenderTargets receiver: UAV loop bounded by the count",
          self_.read_va(0xCC2C3D, 5) == b"\x4c\x39\x7c\x24\x10",
          "cmpq %r15,0x10(%rsp)")

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

    # --- A6: the shader binding path is inside the recorder, not a command ---
    check("shader binding: recorder 0x0CB5130 is vtable slot +0x340",
          self_.u64(0x03EDFFE0) == 0x0CB5130,
          "slot value")
    check("shader binding: receiver is called from the recorder",
          self_.read_va(0x0CB6502, 5) == b"\xe8\xe9\x5d\x01\x00",
          "callq 0x0CCC2F0 at 0x0CB6502")
    check("shader binding: context+0x468 loads the shader-state object",
          self_.read_va(0x0CCD0D4, 7) == b"\x48\x8b\x80\x68\x04\x00\x00",
          "movq 0x468(%rax),%rax")
    check("shader binding: shader-state+0x200 loads the shader resource",
          self_.read_va(0x0CCD0DB, 7) == b"\x4c\x8b\xa8\x00\x02\x00\x00",
          "movq 0x200(%rax),%r13")
    check("shader binding: shader-resource+0x1D8 loads the program data",
          self_.read_va(0x0CCD0F6, 7) == b"\x4d\x8b\xa5\xd8\x01\x00\x00",
          "movq 0x1d8(%r13),%r12")
    check("shader binding: register array is program_data+0x40 indexed by a byte",
          self_.read_va(0x0CCD12C, 5) == b"\x49\x8d\x54\x9c\x40",
          "leaq 0x40(%r12,%rbx,4),%rdx")
    check("shader binding: pixel-shader setter passes a null array",
          self_.read_va(0x0CCD0BE, 4) == b"\x31\xd2\xe8\x7b",
          "xorl %edx,%edx; callq")

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
