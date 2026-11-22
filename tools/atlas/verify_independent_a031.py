#!/usr/bin/env python3
"""A-031: Verify original Déraciné 1.00 SELF grouped binding dispatcher.

Usage: python verify_independent_a031.py PRIVATE_ORIGINAL_EBOOT.bin
Read-only; SHA-gated. The 110 checks are 64 instruction anchors,
nine direct-call edges, one tail jump, and 36 synthetic arithmetic tests.
"""
import hashlib
import struct
import sys
from pathlib import Path

SIZE = 81_506_767
HASH = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
DELTA = 0x1FD30
TEXT_END = 0x33C9000
ANCHORS = """CBFFC5|498b842468040000
CBFFCD|488bb0e0010000
CBFFD9|8b9680030000
CBFFEF|488bb0e8010000
CC000B|488bb0f0010000
CC0027|488bb000020000
CC0087|418b8c2428090000
CC0093|238e00020000
CC00B1|4c8b8048020000
CC00D0|c4e268f3d9
CC013D|4963dd
CC0140|498b84dc68050000
CC0150|4c8b7820
CC016D|418b0488
CC0190|0fb6d8
CC0193|c4e240f7c0
CC0198|498b3cc7
CC01AA|488b07
CC01AD|ff5038
CC01B4|4889c6
CC01B7|89da
CC01BC|e8ef110000
CC01D4|c1e918
CC01D7|4439e9
CC01F8|488b8808020000
CC01FF|8b1499
CC0210|0fb6d0
CC0213|c4e240f7c0
CC021E|498b34c7
CC0222|e809befeff
CC0234|c1e918
CC026F|488b8918020000
CC0276|8b1499
CC0290|0fb6d0
CC0293|c4e240f7c0
CC0298|498bbc2460040000
CC02A0|b901000000
CC02A5|4d8b04c7
CC02AF|4983c014
CC02B3|e8a82a2a02
CC02CA|c1e918
CC02E1|41c784242809000000000000
CC0314|418b8f34090000
CC031B|238e00020000
CC0327|4c8b8648020000
CC0684|418b8f2c090000
CC068B|238e00020000
CC0697|4c8b8648020000
CC09F4|418b8f38090000
CC09FB|238e00020000
CC0A07|4c8b8648020000
CC064B|41c7873409000000000000
CC09BB|41c7872c09000000000000
CC0D2B|41c7873809000000000000
CC13BE|803dc52faf0301
CC13D3|4983c518
CC13DC|498b7d18
CC13E4|e837193d02
CC14B6|bfe8020000
CC14BB|4d8b4500
CC14C4|4489f6
CC14C7|4489fa
CC14CA|4903bc2460040000
CC14E0|e95b152a02"""
CALLS = {
    0xCBFFE2: 0xCC0070,
    0xCBFFFE: 0xCC0300,
    0xCC001A: 0xCC0670,
    0xCC0036: 0xCC09E0,
    0xCC003E: 0xCC0D50,
    0xCC01BC: 0xCC13B0,
    0xCC0222: 0xCAC030,
    0xCC02B3: 0x2F62D60,
    0xCC13E4: 0x3092D20,
}
TAIL_JUMPS = {0xCC14E0: 0x2F62A40}

def unpack_binding(word):
    return (word & 255, (word >> 8) & 65535, word >> 24)

def main(path):
    b = Path(path).read_bytes()
    if len(b) != SIZE or hashlib.sha256(b).hexdigest() != HASH or b[:4] != bytes.fromhex("4f153d1d"):
        raise ValueError("REFUSED: original Déraciné 1.00 SELF identity mismatch")
    def read(va, n):
        if not (0 <= va and va + n <= TEXT_END):
            raise ValueError("VA outside original executable segment")
        return b[va+DELTA:va+DELTA+n]
    checks = []
    for line in ANCHORS.splitlines():
        va_hex, bytes_hex = line.split("|")
        va = int(va_hex, 16)
        expected = bytes.fromhex(bytes_hex)
        checks.append((f"opcode {va:#x}", read(va, len(expected)) == expected))
    for op, mapping, label in ((0xE8, CALLS, "call"), (0xE9, TAIL_JUMPS, "tail")):
        for va, target in mapping.items():
            delta = struct.unpack("<i", read(va+1, 4))[0]
            checks.append((f"{label} {va:#x}->{target:#x}",
                          read(va, 1) == bytes([op]) and va+5+delta == target))
    for slot, index, group in ((0,0,0),(1,2,3),(8,0x1234,31),(255,65535,255)):
        word = slot | (index << 8) | (group << 24)
        checks.append((f"synthetic entry {word:#x}",
                       unpack_binding(word) == (slot,index,group)))
    for k in range(32):
        word = 1 << k
        checks.append((f"synthetic isolated mask {k}", (word & -word).bit_length()-1 == k))
    passed = sum(ok for _,ok in checks)
    for label,ok in checks:
        if not ok: print("FAIL",label)
    print(f"A-031 approved original SELF: {passed}/{len(checks)} checks passed")
    return int(passed != len(checks))

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__,file=sys.stderr)
        sys.exit(2)
    try:
        sys.exit(main(sys.argv[1]))
    except (ValueError,OSError,struct.error) as err:
        print(str(err),file=sys.stderr)
        sys.exit(2)
