#!/usr/bin/env python3
"""A5: read function bounds from .eh_frame_hdr.

The PT_GNU_EH_FRAME segment in this binary holds an .eh_frame_hdr with a binary
search table of (initial_location, fde_address) pairs. That gives independently
confirmed function start addresses without decoding CIE/FDE records.

Header layout (DWARF):
  byte 0  version
  byte 1  eh_frame_ptr_enc
  byte 2  fde_count_enc
  byte 3  table_enc
  then    eh_frame_ptr, fde_count, then fde_count pairs
"""
import struct
import sys

PT_LOAD = 1
PT_GNU_EH_FRAME = 0x6474E550

# DW_EH_PE encodings used by this binary.
DW_EH_PE_omit = 0xFF
DW_EH_PE_udata4 = 0x03
DW_EH_PE_sdata4 = 0x0B
DW_EH_PE_pcrel = 0x10
DW_EH_PE_datarel = 0x30


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


def read_encoded(data, pos, enc, base):
    """Read one encoded pointer; return (value, new_pos)."""
    fmt = enc & 0x0F
    app = enc & 0x70
    if fmt == 0x03:
        raw = struct.unpack_from("<I", data, pos)[0]
        pos += 4
    elif fmt == 0x0B:
        raw = struct.unpack_from("<i", data, pos)[0]
        pos += 4
    elif fmt == 0x04:
        raw = struct.unpack_from("<Q", data, pos)[0]
        pos += 8
    elif fmt == 0x0C:
        raw = struct.unpack_from("<q", data, pos)[0]
        pos += 8
    else:
        raise SystemExit(f"unsupported pointer encoding {enc:#x}")
    if app == DW_EH_PE_pcrel:
        raw += base + pos
    elif app == DW_EH_PE_datarel:
        raw += base
    return raw, pos


def main():
    path = sys.argv[1]
    data, segs = load(path)
    hdr = next((s for s in segs if s["type"] == PT_GNU_EH_FRAME), None)
    if hdr is None:
        raise SystemExit("no PT_GNU_EH_FRAME")

    base_off = hdr["off"]
    base_va = hdr["va"]
    version = data[base_off]
    eh_ptr_enc = data[base_off + 1]
    count_enc = data[base_off + 2]
    table_enc = data[base_off + 3]
    print(f"file={path}")
    print(f"  version={version} eh_frame_ptr_enc={eh_ptr_enc:#x} "
          f"fde_count_enc={count_enc:#x} table_enc={table_enc:#x}")

    pos = base_off + 4
    eh_ptr, pos = read_encoded(data, pos, eh_ptr_enc, base_va)
    count, pos = read_encoded(data, pos, count_enc, base_va)
    print(f"  eh_frame_ptr={eh_ptr:#x} fde_count={count}")

    starts = []
    for _ in range(count):
        loc, pos = read_encoded(data, pos, table_enc, base_va)
        fde, pos = read_encoded(data, pos, table_enc, base_va)
        starts.append(loc)
    starts.sort()
    print(f"  parsed {len(starts)} function starts")

    if len(sys.argv) > 2:
        for q in sys.argv[2:]:
            target = int(q, 16)
            # The containing function is the greatest start <= target.
            lo, hi = 0, len(starts) - 1
            best = None
            while lo <= hi:
                mid = (lo + hi) // 2
                if starts[mid] <= target:
                    best = starts[mid]
                    lo = mid + 1
                else:
                    hi = mid - 1
            if best is None:
                print(f"  {target:#x}: before the first known function")
            else:
                idx = starts.index(best)
                nxt = starts[idx + 1] if idx + 1 < len(starts) else None
                end = f"{nxt:#x}" if nxt else "end"
                print(f"  {target:#x}: function start {best:#x}, next start {end}")


if __name__ == "__main__":
    main()
