#!/usr/bin/env python3
"""C-011: original SELF digest, ABI and producer/consumer edges. No game bytes embedded."""
import hashlib, pathlib, sys
p=pathlib.Path(sys.argv[1])
with p.open('rb') as f:
    assert hashlib.file_digest(f,'sha256').hexdigest()=='8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
    b={}
    for va,n,d in [(0x91c0,0x160,'93430d415991cb48f1634be388544d6075898a4135f3bc129bee7f69d0ecac80'),(0x9530,0x150,'87e90374c7c88bebd5e814130b6d6ddc77cd44c4dffec1525dc2f02c03d106c9')]:
        f.seek(va+0x1fd30);v=f.read(n);assert hashlib.sha256(v).hexdigest()==d;b[va]=v
def read(addr,n):
    va=next(k for k,v in b.items() if k<=addr and addr+n<=k+len(v))
    return b[va][addr-va:addr-va+n]
for site,hexbytes in {0x91df:'4889fb',0x91e2:'4989cc',0x91e5:'4989d6',0x91e8:'4989f5',0x954f:'4889fb',0x9552:'4989cc',0x9555:'4989d6',0x9558:'4989f5',0x92d9:'4c89f7',0x92dc:'4889de',0x92df:'4c89ea',0x92e2:'4c89e1',0x9649:'4c89f7',0x964c:'4889de',0x964f:'4c89ea',0x9652:'4c89e1'}.items():
    assert read(site,len(bytes.fromhex(hexbytes)))==bytes.fromhex(hexbytes),hex(site)
for site,target in [(0x924a,0xad0),(0x9281,0x9c0),(0x9293,0x3020),(0x92ca,0x5760),(0x92e5,0x6190),(0x95ba,0xad0),(0x95f1,0x9c0),(0x9603,0x3020),(0x963a,0x5760),(0x9655,0x6f40)]:
    raw=read(site,5);assert raw[0]==0xe8 and site+5+int.from_bytes(raw[1:],'little',signed=True)==target,hex(site)
print('C-011 original SELF, entry ABI, and ten call edges: PASS')
