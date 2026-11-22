#!/usr/bin/env python3
"""A-023 read-only, SHA-gated verifier for original Déraciné 1.00 SELF.

Usage: python verify_independent_a023.py PRIVATE_ORIGINAL_EBOOT.bin
No game data is written, emitted, or redistributed.
"""
import hashlib
import struct
import sys
from pathlib import Path

EXPECTED_SIZE = 81_506_767
EXPECTED_HASH = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
CODE_DELTA = 0x1FD30
RELRO_DELTA = 0x21240

ANCHORS = [
 (0xCB12F7, "488b9b60040000", "direct bind state object from context+0x460"),
 (0xCB12FE, "498b86d8010000", "direct bind program data +0x1d8"),
 (0xCB1305, "498d8eb0000000", "direct bind shader resource+0xb0"),
 (0xCB132A, "48898b68c40000", "direct bind stage-zero map pointer store"),
 (0xCB1331, "48898388c30000", "direct bind cached program data"),
 (0xCB13B1, "488b8368c40000", "direct bind re-read map"),
 (0xCC369C, "4889442428", "alternate bind saved owner pointer"),
 (0xCC36A1, "488b5848", "alternate bind wrapper"),
 (0xCC36B3, "4c8b7b60", "alternate bind shader resource via +0x60"),
 (0xCC36F7, "488b442430", "alternate bind graphics state"),
 (0xCC36FC, "498d8fb0000000", "alternate bind shader resource+0xb0"),
 (0xCC372B, "48898b68c40000", "alternate bind stage-zero map pointer store"),
 (0xCC387E, "4c8b642428", "separate upload owner from saved stack"),
 (0xCC388F, "498d9424bc0000", "owner+0xbc range header"),
 (0xCC389C, "498d9424c20000", "owner+0xc2 range header"),
 (0xCC38A7, "4889de", "upload source identity in rsi"),
 (0xCC38AA, "488d8c24c0000000", "upload stack payload source"),
 (0xCC38B2, "e8e9320000", "upload helper call"),
 (0xCC6BB1, "0fb74204", "upload length at header+4"),
 (0xCC6BB5, "bb10000000", "maximum copied bytes 0x10"),
 (0xCC6BD1, "0fb702", "first 16-bit header field"),
 (0xCC6BDF, "0fb74202", "second 16-bit header field"),
 (0xCC6E37, "41ff442414", "command count increment"),
 (0xCC6E40, "498b4c2408", "get tail slot"),
 (0xCC6E45, "488901", "append linked command"),
 (0xCC6E48, "488d0d91892103", "vptr lea"),
 (0xCC6E4F, "4989542408", "advance tail slot"),
 (0xCC6E54, "48c7400800000000", "clear Next"),
 (0xCC6E5C, "488908", "store command vptr"),
 (0xCC6E5F, "4c894810", "store source identity"),
 (0xCC6E63, "4c897018", "store copied payload pointer"),
 (0xCC6E67, "8b4dc8", "store first header field"),
 (0xCC6E6D, "8b4dcc", "store second header field"),
 (0xCC6E73, "895828", "store copied byte count"),
 (0xCAB204, "488b4620", "Execute list context"),
 (0xCAB208, "488b7718", "Execute copied data"),
 (0xCAB20C, "0fb74f28", "Execute byte count"),
 (0xCAB210, "0fb75724", "Execute destination offset"),
 (0xCAB214, "488b80080b0000", "Execute destination context+0xb08"),
 (0xCAB21F, "e98cfcffff", "Execute common copy tail jump"),
 (0xCAAEC7, "498b7c2448", "destination backing pointer"),
 (0xCAAECF, "83f940", "copy short-path max threshold"),
 (0xCAB0D0, "418b4c2450", "destination high-water read"),
 (0xCAB0E1, "4189442450", "destination high-water write"),
 (0xCAB0E6, "41c644244001", "destination dirty flag"),
]

def check_file(path):
    data = Path(path).read_bytes()
    if len(data) != EXPECTED_SIZE or hashlib.sha256(data).hexdigest() != EXPECTED_HASH:
        raise ValueError("REFUSED: image is not the approved original Déraciné 1.00 SELF")
    def code(va, n):
        if not 0 <= va < 0x033C9000 or va + n > 0x033C9000:
            raise ValueError("VA outside checked executable window")
        return data[va + CODE_DELTA:va + CODE_DELTA + n]
    def qword(va):
        return struct.unpack_from("<Q", data, va + RELRO_DELTA)[0]

    checks = []
    for va, hexbytes, desc in ANCHORS:
        expected = bytes.fromhex(hexbytes)
        checks.append((f"{va:#x}: {desc}", code(va, len(expected)) == expected))
    rel = struct.unpack("<i", code(0xCC38B3, 4))[0]
    checks.append(("upload calls 0xCC6BA0", 0xCC38B2 + 5 + rel == 0xCC6BA0))
    rel = struct.unpack("<i", code(0xCC6E4B, 4))[0]
    checks.append(("upload creates vptr 0x03EDF7E0", 0xCC6E48 + 7 + rel == 0x03EDF7E0))
    checks.append(("upload command Execute target", qword(0x03EDF7E0) == 0x0CAB200))
    checks.append(("upload command second virtual", qword(0x03EDF7E8) == 0x0CAB230))

    siblings = [
        (0x03EDF740, 0x0CAAE70, 0x0CAAEA0, 0xAE0),
        (0x03EDF760, 0x0CAB100, 0x0CAB130, 0xAE8),
        (0x03EDF780, 0x0CAB140, 0x0CAB170, 0xAF0),
        (0x03EDF7A0, 0x0CAB180, 0x0CAB1B0, 0xB00),
        (0x03EDF7C0, 0x0CAB1C0, 0x0CAB1F0, 0xAF8),
        (0x03EDF7E0, 0x0CAB200, 0x0CAB230, 0xB08),
    ]
    prefix = bytes.fromhex("554889e5488b4620488b77180fb74f280fb75724488b80")
    for vt, exe, second, destination in siblings:
        checks.append((f"{vt:#x}: Execute pointer",qword(vt)==exe))
        checks.append((f"{vt:#x}: second virtual pointer",qword(vt+8)==second))
        checks.append((f"{exe:#x}: argument-routing instruction prefix",code(exe,len(prefix))==prefix))
        checks.append((f"{exe:#x}: destination member +{destination:#x}",
                       code(exe+len(prefix),4)==struct.pack("<I",destination)))
        jmp_addr = exe + 0x1F
        rel = struct.unpack("<i",code(jmp_addr+1,4))[0]
        checks.append((f"{exe:#x}: tail jump to 0xCAAEB0",
                       code(jmp_addr,1)==b"\xe9" and jmp_addr+5+rel==0xCAAEB0))

    passed = sum(ok for _,ok in checks)
    for name, ok in checks:
        if not ok:
            print("FAIL:", name)
    print(f"A-023 original SELF: {passed}/{len(checks)} checks passed")
    return int(passed != len(checks))

if __name__ == "__main__":
    if len(sys.argv)!=2:
        print(__doc__,file=sys.stderr)
        sys.exit(2)
    try:
        sys.exit(check_file(sys.argv[1]))
    except (OSError,ValueError,struct.error) as err:
        print(str(err),file=sys.stderr)
        sys.exit(2)
