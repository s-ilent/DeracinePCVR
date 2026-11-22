#!/usr/bin/env python3
"""A-027 approved original Déraciné 1.00 SELF verification; read-only.
Usage: python verify_independent_a027.py <private-eboot.bin>
"""
from pathlib import Path
import hashlib, struct, sys
SIZE=81506767
SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
DELTA=0x1fd30
TABLE=0x35c9cfc
ANCHORS="""02F5D2BF|8b4a08
02F5D2C2|8b420c
02F5D2C5|448b0a
02F5D2C8|48c1e108
02F5D2CC|48c1e028
02F5D2D3|4181e1ffff7f00
02F5D2DA|4809c8
02F5D2E0|450fb65401f1
02F5D2E6|410fb64c01f0
02F5D354|48c1e102
02F5D358|41bd03000000
02F5D35E|498d4401e4
02F5D36A|488d14b500000000
02F5D372|4c8d1d83c96600
02F5D386|4929cd
02F5D389|4901c5
02F5D392|4889f1
02F5D395|48f7d9
02F5D3B0|410fb65c8dff
02F5D3B6|450fb6548dfd
02F5D3D4|4983fa25
02F5D3DA|410fb6548d00
02F5D3E0|4b630493
02F5D3E4|450fb67c8dfe
02F5D551|885f12
02F5D554|48ffc1
02F5D557|0f8553feffff
02F5D55F|885f13
02F5D562|48ffc1
02F5D565|0f8545feffff
02F5D56D|885f14
02F5D570|48ffc1
02F5D573|0f8537feffff
02F5D57B|885f15
02F5D57E|48ffc1
02F5D581|0f8529feffff
02F5D435|6646898c7fa2000000
02F5D46B|6646898c7fc2000000
02F5D49F|6646898c7fea000000
02F5D5B8|664289947f82000000
02F5D630|428a1493
02F5D634|80c2ed
02F5D637|80fa06"""
def locate(code, length, chunk_dw, n):
    if not (0<=length<=0x7fffff and 0<=chunk_dw<=255 and 0<=n<=255):
        raise ValueError("bad fixture")
    return code+length-0x1c-4*(chunk_dw+n)
def verify(file):
    b=Path(file).read_bytes()
    if len(b)!=SIZE or hashlib.sha256(b).hexdigest()!=SHA or b[:4]!=bytes.fromhex("4f153d1d"):
        raise ValueError("REFUSED: wrong original Déraciné 1.00 SELF")
    def read(va,n):
        if va<0 or va+n>0x3e86d30:raise ValueError("address out of text range")
        return b[va+DELTA:va+DELTA+n]
    checks=[]
    for line in ANCHORS.splitlines():
        a,h=line.split("|"); expected=bytes.fromhex(h)
        checks.append((f"instruction {a}",read(int(a,16),len(expected))==expected))
    for kind,target in ((0x22,0x2f5d551),(0x23,0x2f5d55f),(0x24,0x2f5d56d),(0x25,0x2f5d57b)):
        offset=struct.unpack("<i",read(TABLE+kind*4,4))[0]
        checks.append((f"type {kind:#x}",TABLE+offset==target))
    code=0x1234567000;end=code+0x100;footer=end-28
    checks.append(("footer + 0xc",footer+12==end-16))
    checks.append(("footer + 0xd",footer+13==end-15))
    checks.append(("record pointer algebra",locate(code,256,2,3)==footer-20))
    mem=bytearray(0x1100)
    f=0x1000+256-28;mem[f:f+7]=b"OrbShdr";mem[f+12]=2;mem[f+13]=3
    sample=bytes((0x22,2,7,0,2,0,16,0,0x25,1,4,0))
    pos=locate(0x1000,256,mem[f+12],mem[f+13])
    mem[pos:pos+12]=sample
    values=[tuple(mem[pos+i*4:pos+i*4+4]) for i in range(3)]
    checks.append(("synthetic input usage slots",values==[(0x22,2,7,0),(2,0,16,0),(0x25,1,4,0)]))
    checks.append(("kind 0x22 store uses bl",read(0x2f5d551,3)==bytes.fromhex("885f12")))
    passed=sum(v for _,v in checks)
    for label,valid in checks:
        if not valid:print("FAIL",label)
    print(f"A-027 original SELF: {passed}/{len(checks)} checks passed")
    return 0 if passed==len(checks) else 1
if __name__=="__main__":
    if len(sys.argv)!=2:print(__doc__,file=sys.stderr);sys.exit(2)
    try:sys.exit(verify(sys.argv[1]))
    except (OSError,ValueError,struct.error) as e:print(e,file=sys.stderr);sys.exit(2)
