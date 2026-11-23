#!/usr/bin/env python3
"""C-012: verify paired descriptor normalizers on exact original SELF; no proprietary bytes embedded."""
import hashlib, pathlib, struct, sys
p=pathlib.Path(sys.argv[1])
with p.open("rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest()=="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
    windows={}
    for va,d in [(0x9460,"6807f100194df049271939d4d92026d1a92bbcfaada1b2a7108a00fd9c70e4b4"),(0x97d0,"5a31822fba57362a4b65ee594364e8e1248a8ab2e7649f63e54d0c8bf8245c6f")]:
        f.seek(va+0x1fd30); b=f.read(200)
        assert hashlib.sha256(b).hexdigest()==d
        windows[va]=b
def at(va,n):
    k=next(k for k,b in windows.items() if k<=va and va+n<=k+len(b))
    return windows[k][va-k:va-k+n]
for site,target in [(0x950c,0x91c0),(0x987c,0x9530)]:
    b=at(site,5);assert b[0]==0xe8 and site+5+struct.unpack("<i",b[1:])[0]==target
for site,h in {0x948d:"448b4208",0x9495:"448b4a0c",0x949d:"8b4210",0x94a3:"807a2c00",0x94a9:"8b4a28",0x9504:"450fafc8",0x9508:"488d4dd0",0x97fd:"448b4208",0x9805:"448b4a0c",0x980d:"8b4210",0x9813:"807a2c00",0x9819:"8b4a28",0x9874:"450fafc8",0x9878:"488d4dd0"}.items():
    assert at(site,len(bytes.fromhex(h)))==bytes.fromhex(h),hex(site)
for site,target in [(0x94b8,0x33cad10),(0x9828,0x33cad90)]:
    b=at(site,7);assert b[:3]==bytes.fromhex("488d1d") and site+7+struct.unpack("<i",b[3:])[0]==target
print("C-012 original SELF, paired wrapper fields, jump-table bases and direct calls: PASS")
