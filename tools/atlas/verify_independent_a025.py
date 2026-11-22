#!/usr/bin/env python3
"""A-025 independent, read-only shader metadata constructor verifier.
Usage: python verify_independent_a025.py /path/to/approved/original/eboot.bin
"""
import hashlib
from pathlib import Path
import struct
import sys

SIZE = 81_506_767
SHA256 = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
CODE_DELTA = 0x1FD30
# Original-SELF anchored instruction prefixes: inner-ELF VA|bytes|semantic role.
ANCHORS = """
02F5D290|55|push rbp
02F5D2B2|c5fcc2c00f|initialize to all-ones
02F5D2BF|8b4a08|read program metadata +8
02F5D2C2|8b420c|read program metadata +0xc
02F5D2C5|448b0a|read program metadata +0
02F5D2C8|48c1e108|pointer reconstruct shift
02F5D2CC|48c1e028|pointer reconstruct shift
02F5D2D3|4181e1ffff7f00|pointer segment mask
02F5D2E0|450fb65401f1|header byte at -0xf
02F5D2E6|410fb64c01f0|header byte at -0x10
02F5D2EC|c5fc1187f80000|metadata initialization
02F5D2F4|c5fc1107|metadata initialization base
02F5D300|c5fc1187c00000|metadata initialization map
02F5D327|c7871201000030|metadata default fields
02F5D338|89b5acfdffff|save mode
02F5D33E|40887703|metadata mode byte
02F5D342|80e301|pointer flag
02F5D348|4c89d6|record count
02F5D34B|4885f6|empty stream
02F5D35E|498d4401e4|record pointer base
02F5D372|4c8d1d83c96600|jump table at 0x35c9cfc
02F5D3B0|410fb65c8dff|record register index
02F5D3B6|450fb6548dfd|record kind
02F5D3BC|83fb0f|index threshold
02F5D3C6|66450f46c8|index branch
02F5D3DA|410fb6548d00|record flags
02F5D3E0|4b630493|jump table signed relative
02F5D3E4|450fb67c8dfe|record logical slot
02F5D3ED|80e202|flag bit
02F5D3FA|c1e60d|flag shift
02F5D3FD|ffe0|indirect dispatch
02F5D44E|4531e4|record kind 2 handler
02F5D45A|0d00800000|set high bit
02F5D45F|80fb10|index 0x10 threshold
02F5D462|66440f42c8|select encoded index
02F5D46B|6646898c7fc200|write metadata +0xc2 map
02F5D49F|6646898c7fea00|write separate +0xea map
02F5D594|664289547f22|write another metadata table
02F5D5B8|664289947f8200|write another metadata table
02F5D5D2|4c8bad98fdffff|decode loop cleanup
02F5D5D9|4501f4|accumulate word count
02F5DDB5|66898c57c20000|alternate +0xc2 write
02F5E078|49ffc2|record increment
02F5E084|66448907|metadata final count
02F6246E|4c8b8cd780c100|consumer effective stage map
02F624B2|410fb78441c200|consumer metadata +0xc2 map
02F624BB|3dffff0000|consumer unmapped sentinel
02F624C9|4181e5ff3f0000|consumer mapped low 14 bits
00CEFCF3|4d89e2|special resource base
00CEFD07|4981c268020000|special metadata +0x268
00CF00FE|be04000000|mode4
00CF0103|4c89d7|mode4 destination
00CF0106|4c89f2|mode4 source
00CF0124|e867d12602|mode4 decoder call
00CF026A|be02000000|mode2
00CF026F|4881c7b0000000|mode2 metadata +0xb0
00CF0276|49899424d80100|mode2 shader program
00CF0297|e8f4cf2602|mode2 decoder call
00CF1D40|4d89ac24d80100|mode1 shader program
00CF1D48|4c89e7|mode1 resource
00CF1D4B|be01000000|mode1
00CF1D50|4c89ea|mode1 source
00CF1D5B|4881c7b0000000|mode1 metadata +0xb0
00CF1D75|e816b52602|mode1 decoder call
00CF2B30|4d89ac24d80100|mode5 shader program
00CF2B38|4c89e7|mode5 resource
00CF2B3B|be05000000|mode5
00CF2B40|4c89ea|mode5 source
00CF2B4B|4881c7b0000000|mode5 metadata +0xb0
00CF2B65|e826a72602|mode5 decoder call
00CF47B0|4d89bc24d80100|mode3 shader program
00CF47B8|4c89e7|mode3 resource
00CF47BB|be03000000|mode3
00CF47D0|4881c7b0000000|mode3 metadata +0xb0
00CF47FF|4c89fa|mode3 source
00CF4807|e8848a2602|mode3 decoder call
00CF5160|4d89b424d80100|mode0 shader program
00CF5168|4c89e7|mode0 resource
00CF516B|31f6|mode0
00CF5178|4881c7b0000000|mode0 metadata +0xb0
00CF5192|e8f9802602|mode0 decoder call
"""

CALLERS = [
    (0x0CF0124, 4, 0x0CF00FE),
    (0x0CF0297, 2, 0x0CF026A),
    (0x0CF1D75, 1, 0x0CF1D4B),
    (0x0CF2B65, 5, 0x0CF2B3B),
    (0x0CF4807, 3, 0x0CF47BB),
    (0x0CF5192, 0, 0x0CF516B),
]
JUMP_TABLE = 0x035C9CFC

def verify(path):
    data = Path(path).read_bytes()
    if (len(data) != SIZE or hashlib.sha256(data).hexdigest() != SHA256
            or data[:4] != bytes.fromhex("4f153d1d")):
        raise ValueError("REFUSED: unapproved original Déraciné 1.00 SELF")
    def code(va, n):
        if va < 0 or va + n > 0x033C9000:
            raise ValueError("outside verified executable segment")
        return data[va+CODE_DELTA:va+CODE_DELTA+n]
    checks = []
    for line in ANCHORS.strip().splitlines():
        address, expected_hex, description = line.split("|", 2)
        va = int(address, 16)
        expected = bytes.fromhex(expected_hex)
        checks.append((f"{va:#x} {description}", code(va, len(expected)) == expected))
    for site, mode, mode_site in CALLERS:
        rel = struct.unpack("<i", code(site+1,4))[0]
        checks.append((f"{site:#x} calls decoder",
                       code(site,1) == b"\xe8" and site+5+rel == 0x02F5D290))
        checks.append((f"{site:#x} passes decoder mode {mode}",
                       code(mode_site, 5) == b"\xbe"+mode.to_bytes(4,"little") if mode
                       else code(mode_site,2) == b"\x31\xf6"))
    for kind, target in [(2,0x02F5D44E),(3,0x02F5D482)]:
        rel = struct.unpack_from("<i",data,JUMP_TABLE+CODE_DELTA+kind*4)[0]
        checks.append((f"kind {kind} dispatch", JUMP_TABLE+rel == target))
    checks.append(("kind range 0..0x25",code(0x02F5D3D4,4)==bytes.fromhex("4983fa25")))
    checks.append(("special metadata offset +0x268",
                   code(0x0CEFD07,7)==bytes.fromhex("4981c268020000")))
    passed = sum(ok for _,ok in checks)
    for label,ok in checks:
        if not ok: print("FAIL:",label)
    print(f"A-025 original SELF: {passed}/{len(checks)} checks passed")
    return 0 if passed == len(checks) else 1

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__,file=sys.stderr);sys.exit(2)
    try: sys.exit(verify(sys.argv[1]))
    except (OSError, ValueError, struct.error) as e:
        print(e,file=sys.stderr);sys.exit(2)
