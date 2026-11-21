#!/usr/bin/env python3
"""Read-only Déraciné 1.00 SELF check for RHI indexed-indirect draw.
Usage: python tools/atlas/verify_indexed_indirect.py /path/to/eboot.bin
No ELF export, runtime execution, guest data modification, or third-party deps.
"""
import bisect
import hashlib
import os
import struct
import sys

EXPECTED = '8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
u32 = lambda b, i=0: struct.unpack_from('<I', b, i)[0]
u64 = lambda b, i=0: struct.unpack_from('<Q', b, i)[0]

def verify(path):
    assert os.path.getsize(path) == 81506767
    with open(path, 'rb') as f:
        assert hashlib.file_digest(f, 'sha256').hexdigest() == EXPECTED
    fd = os.open(path, os.O_RDONLY)
    try:
        rd = lambda o, n: os.pread(fd, n, o)
        head = rd(0, 0x400)
        assert head[:4] == bytes.fromhex('4f153d1d')
        nseg = struct.unpack_from('<H', head, 0x18)[0]
        segments = {}
        for j in range(nseg):
            flags, off, size, _ = struct.unpack_from('<QQQQ', head, 0x20+j*32)
            if flags & 0xfffff == 0x2804:
                segments[flags >> 20] = off, size
        eo = 0x20+nseg*32
        assert head[eo:eo+4] == b'\x7fELF'
        phoff = u64(head, eo+0x20)
        ents, count = struct.unpack_from('<HH', head, eo+0x36)
        ph = [struct.unpack('<IIQQQQQQ', rd(eo+phoff+j*ents, ents)) for j in range(count)]
        def elf_off(o, n):
            for j, p in enumerate(ph):
                if j in segments and p[2] <= o and o+n <= p[2]+p[5] and o-p[2]+n <= segments[j][1]:
                    return rd(segments[j][0]+o-p[2], n)
            raise ValueError('Unmapped ELF file offset')
        def va(a, n):
            for j, p in enumerate(ph):
                if j in segments and p[3] <= a and a+n <= p[3]+p[5] and a-p[3]+n <= segments[j][1]:
                    return rd(segments[j][0]+a-p[3], n)
            raise ValueError('Unmapped ELF VA')
        dyn = next(p for p in ph if p[0] == 2)
        tags = {}
        d = elf_off(dyn[2], dyn[5])
        for i in range(0, len(d), 16):
            tag, val = struct.unpack_from('<QQ', d, i)
            if tag == 0: break
            tags[tag] = val
        db = next(p for p in ph if p[0] == 0x61000000)
        def dynread(offset, size): return elf_off(db[2]+offset, size)
        def relocation(slot, start_tag, size_tag):
            b = dynread(tags[start_tag], tags[size_tag]); matches = []
            for i in range(0, len(b), 24):
                r, info, add = struct.unpack_from('<QQq', b, i)
                if r == slot: matches.append((i//24, info >> 32, info & 0xffffffff, add))
            assert len(matches) == 1
            return matches[0]
        base, slot, method, call, stub, got = (0x3edfca0, 0x3ee0198, 0xcbbef0,
                                               0xcbc359, 0x33c8390, 0x4381da8)
        assert slot-base == 0x4f8
        assert u64(va(base+0x4f0, 8)) == 0xcbb9c0
        assert u64(va(slot, 8)) == method
        assert relocation(slot, 0x6100002f, 0x61000031) == (37982, 0, 8, method)
        eh = next(p for p in ph if p[0] == 0x6474e550)
        haddr = eh[3]; header = va(haddr, 12)
        assert header[:4] == bytes([1, 0x1b, 3, 0x3b])
        n = u32(header, 8); table = va(haddr+12, n*8)
        starts = [haddr+struct.unpack_from('<i', table, j*8)[0] for j in range(n)]
        i = bisect.bisect_left(starts, method)
        assert i < n and starts[i] == method
        fde = haddr+struct.unpack_from('<i', table, i*8+4)[0]
        length, cie, delta, extent = struct.unpack('<IIiI', va(fde, 16))
        assert fde+8+delta == method and extent == 0x5e7
        assert method <= call < method+extent
        # Independent instruction-boundary checks near the already FDE-bounded call.
        for address, op in [(0xcbbf01, '4c8bb760040000'), (0xcbc32f, '498b3c24'),
                            (0xcbc33c, 'be09000000'), (0xcbc351, 'b906000000'),
                            (0xcbc356, '8b55cc'), (0xcbc362, '4983042424'),
                            (0xcbc371, '498b9d60040000'), (0xcbc3b5, '41c785d004000001000000')]:
            assert va(address, len(op)//2) == bytes.fromhex(op), hex(address)
        e8 = va(call, 5)
        assert e8[0] == 0xe8 and call+5+struct.unpack_from('<i', e8, 1)[0] == stub
        plt = va(stub, 6)
        assert plt[:2] == bytes.fromhex('ff25')
        assert stub+6+struct.unpack_from('<i', plt, 2)[0] == got
        jindex, sym, typ, add = relocation(got, 0x61000029, 0x6100002d)
        assert jindex == 451 and typ == 7
        symbols = dynread(tags[0x61000039], tags[0x6100003d]); strings = dynread(tags[0x61000035], tags[0x61000037])
        name_off = u32(symbols, 24*sym)
        name = strings[name_off:strings.index(b'\0',name_off)].decode('ascii')
        assert name == 'ED9-Fjr8Ta4#A#B'
        bodyhash = hashlib.sha256(va(method, extent)).hexdigest()
        assert bodyhash == '53fbfe5da0dbe6a7e6bf85e37a0062fbbfb1557247c7506bcd3a6e74c5344bc5'
        print('PASS: SELF SHA; RELATIVE +0x4f8 slot; exact FDE; decoded draw args;')
        print('      GNM rel32 -> PLT -> JUMP_SLOT NID; original method body SHA')
        print('ELF VA method 0xCBBEF0 FDE [0xCBBEF0,0xCBC4D7); call 0xCBC359')
    finally:
        os.close(fd)

if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: verify_indexed_indirect.py /path/to/eboot.bin')
    verify(sys.argv[1])
