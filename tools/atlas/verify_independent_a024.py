#!/usr/bin/env python3
"""Read-only A-024 original Déraciné SELF checker. Usage: python verify_independent_a024.py path/to/original-eboot.bin"""
from pathlib import Path
import hashlib, struct, sys

SIZE=81506767
SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
DELTA=0x1fd30
ANCHORS="""2F6246E|4c8b8cd780c10000|stage-indexed pointer
2F624B2|410fb78441c20000|user-data slot map
2F624BB|3dffff0000|unmapped sentinel
2F624C9|4181e5ff3f0000|14 bit slot
2F62538|488d35710b7f00|register bank table
2F62543|0fb71456|bank base indexed lookup
CB12FE|498b86d8010000|stage0 program
CB1305|498d8eb0000000|stage0 resource metadata
CB132A|48898b68c40000|stage0 map store
CB4B4E|498b86d8010000|stage0 second program
CB4B55|498d8eb0000000|stage0 second metadata
CB4B7A|48898b68c40000|stage0 second store
CC36FC|498d8fb0000000|stage0 third metadata
CC372B|48898b68c40000|stage0 third store
CBEBFC|498b97e8010000|stage1 resource
CBEC03|4885d2|stage1 null test
CBEC0C|488bb2d8010000|stage1 program
CBEC13|4881c2b0000000|stage1 resource metadata
CBEC1A|498dbde8020000|stage1 writer
CBEC21|e82a372a02|stage1 setter call
CBED30|498dbde8020000|stage1 null writer
CBED37|31f6|stage1 null program
CBED39|31d2|stage1 null metadata
CBED3B|e810362a02|stage1 clear call
2F62423|48899788c10000|stage1 map assignment
CBE39D|498b9f00020000|shader resource A
CBE3AD|4d8ba7e0010000|shader resource B
CBE3B4|498b8c2460020000|resource B conditional
CBE3C5|488b83d8010000|A program
CBE3CC|0fb65003|program selector
CBE3D0|f644905e02|program variant bit
CBE3D5|0f8571020000|variant branch
CBE3EA|498db42468020000|simple stage4 source
CBE41A|4989b588c40000|simple stage4 store
CBE441|488d93b0000000|simple stage3 metadata
CBE448|488bb3d8010000|simple stage3 program
CBE44F|e8fc382a02|simple stage2 and 3 helper
CBE55D|498b87e0010000|fallback B
CBE56D|488b90d8010000|fallback program
CBE589|4c8d90b0000000|fallback stage2 metadata
CBE611|4d899578c40000|fallback stage2 store
CBE6DD|498db42468020000|extended stage4 source
CBE70D|4989b588c40000|extended stage4 store
CBE730|488d8bb0000000|extended stage3 metadata
CBE737|488bb3d8010000|extended program
CBE73E|e89d382a02|extended helper
2F61E00|48899398c10000|simple stage3 map
2F61EB3|4889bb90c10000|simple stage2 map
2F62095|48898b98c10000|extended stage3 map
2F6214E|4c89b390c10000|extended stage2 map
2F61DD8|488dbb80c30000|simple stage2 scratch option 1
2F61E22|488d8368c20000|simple stage2 scratch option 2
2F61E3D|480f44f8|simple selector
2F6206D|4c8db3b0c50000|extended stage2 scratch option 1
2F620B0|488d8398c40000|extended stage2 scratch option 2
2F620CB|4c0f44f0|extended selector"""

def verify(path):
    data=Path(path).read_bytes()
    if len(data)!=SIZE or hashlib.sha256(data).hexdigest()!=SHA or data[:4]!=bytes.fromhex("4f153d1d"):
        raise ValueError("REFUSED: unapproved Déraciné 1.00 SELF")
    def read(va,count):
        if va<0 or va+count>0x03e86d30: raise ValueError("outside validated original image segment")
        return data[va+DELTA:va+DELTA+count]
    checks=[]
    for ln in ANCHORS.strip().splitlines():
        addr,hexbytes,label=ln.strip().split("|",2)
        va=int(addr,16); expected=bytes.fromhex(hexbytes)
        checks.append((label,read(va,len(expected))==expected))
    for source,target in [(0xCBEC21,0x2F62350),(0xCBED3B,0x2F62350),
                          (0xCBE44F,0x2F61D50),(0xCBE73E,0x2F61FE0)]:
        rel=struct.unpack("<i",read(source+1,4))[0]
        checks.append((f"{source:#x} call -> {target:#x}",read(source,1)==b"\xe8" and source+5+rel==target))
    stages=struct.unpack("<7H",read(0x37530B0,14))
    checks.append(("7 shader user-data register bases",list(stages)==[0x2E40,0x2C0C,0x2C4C,0x2C8C,0x2CCC,0x2D0C,0x2D4C]))
    checks.append(("stage pointer slot address arithmetic", all(0x2e8+0xc180+8*i==0xc468+8*i for i in range(5))))
    passed=sum(ok for _,ok in checks)
    for name,ok in checks:
        if not ok:print("FAIL",name)
    print(f"A-024 original SELF: {passed}/{len(checks)} checks passed")
    return 0 if passed==len(checks) else 1

if __name__=="__main__":
    if len(sys.argv)!=2:print(__doc__,file=sys.stderr);sys.exit(2)
    try:sys.exit(verify(sys.argv[1]))
    except (OSError,ValueError,struct.error) as e:print(str(e),file=sys.stderr);sys.exit(2)
