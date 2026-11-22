#!/usr/bin/env python3
"""A-022x read-only verifier: python verify_independent_a022x.py PRIVATE_1.00_EBOOT.bin"""
import hashlib
import struct
import sys
from pathlib import Path

SIZE = 81_506_767
HASH = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
CODE_END = 0x03E86D30
CODE_DELTA = 0x1FD30

# (inner ELF VA, original opcode bytes, semantic anchor)
ANCHORS = [
    (0xCACDAE, "4c8b6620", "A: list+0x20 context"),
    (0xCACDD1, "8bb080030000", "A: stage selector+0x380"),
    (0xCACDD7, "4d8d4750", "A: four-DWORD source+0x50"),
    (0xCACDF5, "bfe8020000", "A: writer offset+0x2e8"),
    (0xCACDFA, "b901000000", "A: one block"),
    (0xCACE0A, "e831562b02", "A: stage-parametric writer call"),
    (0xCACE0F, "4b8dbcf468050000", "A: context cache+0x568"),
    (0xCACE29, "4109842428090000", "A: dirty mask+0x928"),
    (0xCACF94, "4c8b6e20", "B: list+0x20 context"),
    (0xCACFD8, "498d7750", "B: source+0x50"),
    (0xCACFE3, "6685c0", "B: mapping sign bit test"),
    (0xCACFC1, "420fb78470c20000", "B: stage slot lookup"),
    (0xCACFCA, "3dffff0000", "B: 0xffff sentinel"),
    (0xCACFDC, "4181e4ff3f0000", "B: 14-bit mapping"),
    (0xCAD053, "c5f8110488", "B: shadow state write 16B"),
    (0xCAD0C3, "c74008007604c0", "B: SET_SH_REG packet"),
    (0xCAD0BF, "410fb7cc", "B: slot register index"),
    (0xCAD0CA, "89480c", "B: packet register offset"),
    (0xCAD14E, "418b4c245c", "B: resource fourth DWORD"),
    (0xCAD166, "418b4c2450", "B: resource first DWORD"),
    (0xCAD16E, "4883c020", "B: 32B writer advancement"),
    (0xCAD176, "c6823dc5000001", "B: writer dirty"),
    (0xCAD17D, "4b8dbcf508060000", "B: cache+0x608+slot*8"),
    (0xCAD188, "e853e1ffff", "B: refcounted pointer update"),
    (0xCAD197, "4109852c090000", "B: dirty mask+0x92c"),
    (0xCAD1A9, "ffc9", "B: resource reference decrement"),
    (0xCAD1C2, "488b7b28", "B: secondary retained reference"),
    (0xCAD1D4, "e9a7fcffff", "B: common reference release"),
    (0x2F62469, "4189cf", "helper: block count"),
    (0x2F624B2, "410fb78441c20000", "helper: slot map"),
    (0x2F624C9, "4181e5ff3f0000", "helper: 14-bit slot"),
    (0x2F624D5, "c4c1781006", "helper: four-DWORD load"),
    (0x2F624DE, "c4c178110480", "helper: four-DWORD shadow write"),
    (0x2F62538, "488d35710b7f00", "helper: stage-base table"),
    (0x2F6253F, "410fb7cd", "helper: mapped slot"),
    (0x2F62543, "0fb71456", "helper: stage-base table read"),
    (0x2F62557, "8d8c1100d4ffff", "helper: subtract 0x2c00"),
    (0x2F62561, "89480c", "helper: PM4 register offset"),
    (0x2F6257F, "4883c020", "helper: 32B writer advance"),
    (0x2F625A3, "c6840750c2000001", "helper: stage dirty byte"),
]
BASES = [0x2e40,0x2c0c,0x2c4c,0x2c8c,0x2ccc,0x2d0c,0x2d4c]

def main(path):
    data = Path(path).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != SIZE or digest != HASH or data[:4] != bytes.fromhex("4f153d1d"):
        raise ValueError("REFUSED: unapproved image, size or SELF header")
    def read(va,n):
        if not (0 <= va <= CODE_END - n):
            raise ValueError("VA outside verified executable PT_LOAD")
        return data[va+CODE_DELTA:va+CODE_DELTA+n]
    checks = []
    def check(label, condition):
        checks.append((label,condition))
    for va, hx, label in ANCHORS:
        check("%#x %s" % (va,label),read(va,len(bytes.fromhex(hx))).hex() == hx)
    stages = list(struct.unpack("<7H",read(0x37530B0,14)))
    check("original shader-stage register table",stages == BASES)
    check("B fixed base is HS stage",0x2c00 + 0x10c == stages[5])
    pm4 = struct.unpack("<I",read(0xCAD0C6,4))[0]
    check("B PM4 type3",pm4 >> 30 == 3)
    check("B PM4 SET_SH_REG opcode",((pm4 >> 8) & 0xff) == 0x76)
    check("B PM4 four DWORDs",((pm4 >> 16) & 0x3fff) == 4)
    pcrel = struct.unpack("<i",read(0x2F6253B,4))[0]
    check("helper points to stage table",0x2F62538 + 7 + pcrel == 0x37530B0)
    passed = sum(ok for _,ok in checks)
    for name,ok in checks:
        if not ok: print("FAIL:", name)
    print(f"A-022x original SELF: {passed}/{len(checks)} checks passed")
    return int(passed != len(checks))

if __name__ == "__main__":
    if len(sys.argv)<2:
        print(__doc__,file=sys.stderr);sys.exit(2)
    try: sys.exit(main(sys.argv[1]))
    except (OSError,ValueError,struct.error) as err:
        print(str(err),file=sys.stderr);sys.exit(2)
