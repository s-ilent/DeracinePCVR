#!/usr/bin/env python3
"""A-026 approved original Déraciné SELF GNM InputUsageSlot validation.
Usage: python verify_independent_a026.py ORIGINAL_1.00_EBOOT.bin
Read-only: does not emit original executable or shader data.
"""
import hashlib
import struct
import sys
from pathlib import Path

SIZE=81506767
SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
DELTA=0x1FD30
FIRST_TABLE=0x35C9CFC
SECOND_TABLE=0x35C9D94
ANCHORS="""2F5D3B0|410fb65c8dff
2F5D3B6|450fb6548dfd
2F5D3D4|4983fa25
2F5D3DA|410fb6548d00
2F5D3E4|450fb67c8dfe
2F5D3E0|4b630493
2F5D3FF|80fb0f
2F5D418|4531e4
2F5D44E|4531e4
2F5D482|4531e4
2F5D4B6|80fb0f
2F5D440|48ffc1
2F5D594|664289547f22
2F5D435|6646898c7fa2000000
2F5D46B|6646898c7fc2000000
2F5D49F|6646898c7fea000000
2F5D5B8|664289947f82000000
2F5D4CF|885f0d
2F5D4E0|885f0e
2F5D4F1|885f10
2F5D50D|885f0f
2F5D51E|885f04
2F5D52F|885f0c
2F5D540|885f07
2F5D551|885f12
2F5D55F|885f13
2F5D56D|885f14
2F5D57B|885f15
2F5D5F4|4c8d3d99c76600
2F5D630|428a1493
2F5D634|80c2ed
2F5D637|80fa06
2F5D64D|49631497
2F5D643|428a449303
2F5D648|428a4c9302
2F5D654|c0e804
2F5D65B|884f09
2F5D709|884f0b
2F5D7C9|884f08
2F5D8B7|884f05
2F5D8ED|884f0a"""
FIRST="""3FF 418 44E 482 4B6 440 440 4CF 4E0 440 4F1 440 440 50D 440 440 440 440 51E 440 440 440 440 440 440 440 52F 540 440 440 440 440 440 440 551 55F 56D 57B"""
SECOND="""659 E078 707 7C7 8B7 E078 8EB"""

def run(path):
    data=Path(path).read_bytes()
    if len(data)!=SIZE or hashlib.sha256(data).hexdigest()!=SHA or data[:4]!=bytes.fromhex("4f153d1d"):
        raise ValueError("REFUSED: wrong original 1.00 SELF")
    def code(va,n):
        if not (0 <= va and va+n <= 0x03E86D30):
            raise ValueError("VA outside original file-backed executable")
        return data[va+DELTA:va+DELTA+n]
    tests=[]
    for line in ANCHORS.splitlines():
        a,h=line.split("|"); expected=bytes.fromhex(h)
        tests.append((f"opcode 0x{a}", code(int(a,16),len(expected))==expected))
    first=[int(h,16)+0x02F5D000 for h in FIRST.split()]
    # Values E078 in SECOND refer to a different page than the others.
    second=[(0x02F50000+int(h,16)) if h=="E078" else (0x02F5D000+int(h,16)) for h in SECOND.split()]
    for table, targets, label in [(FIRST_TABLE,first,"first"),(SECOND_TABLE,second,"second")]:
        for i,target in enumerate(targets):
            displacement=struct.unpack("<i",code(table+4*i,4))[0]
            tests.append((f"{label} dispatch entry {i}",table+displacement==target))
    tests.append(("first table has 38 cases",len(first)==38))
    tests.append(("second table has 7 cases",len(second)==7))
    tests.append(("constant-buffer map at +0xC2",code(0x2f5d46b,9)==bytes.fromhex("6646898c7fc2000000")))
    tests.append(("vertex-buffer map at +0xEA",code(0x2f5d49f,9)==bytes.fromhex("6646898c7fea000000")))
    n=sum(ok for _,ok in tests)
    for label,ok in tests:
        if not ok: print("FAIL",label)
    print(f"A-026 original SELF: {n}/{len(tests)} checks passed")
    return 0 if n==len(tests) else 1

if __name__=="__main__":
    if len(sys.argv)!=2: print(__doc__,file=sys.stderr);sys.exit(2)
    try: sys.exit(run(sys.argv[1]))
    except (ValueError,OSError,struct.error) as err:
        print(err,file=sys.stderr);sys.exit(2)
