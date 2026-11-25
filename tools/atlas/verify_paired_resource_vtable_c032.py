#!/usr/bin/env python3
import hashlib, struct, sys, bisect
with open(sys.argv[1], 'rb') as f:
    assert hashlib.file_digest(f,'sha256').hexdigest() == '8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
    def read(o,n):
        f.seek(o); return f.read(n)
    base=0x3cfc414; off=base+0x1fd30
    assert read(off,4)==bytes((1,0x1b,3,0x3b))
    n=struct.unpack('<I',read(off+8,4))[0]
    a=[(base+x,base+y) for x,y in struct.iter_unpack('<ii',read(off+12,n*8))]
    i=bisect.bisect_left(a,(0xcb4330,0))
    assert a[i]==(0xcb4330,0x38f73b0)
    assert a[i+1][0]==0xcb5130
    va=0x38f73b0
    l,c,d,r=struct.unpack('<IIiI',read(va+0x1fd30,16))
    assert (l,va+4-c,va+8+d,r)==(32,0x38f4308,0xcb4330,0xe00)
    assert hashlib.sha256(read(0xcb4330+0x1fd30,0xe00)).hexdigest()=='97ed3c973ad8e89089e1a3cdf2439273c388f46f556ebaf9e85422e061bf23d3'
    for j,t in enumerate((0xd01160,0xcb4330,0xcb5130)):
        assert struct.unpack('<Q',read(0x3f01210+j*8,8))[0]==t
        assert struct.unpack('<QQq',read(0x44bacb8+j*24,24))==(0x3edffd0+j*8,8,t)
    site=0xcb438c
    op,rel=struct.unpack('<Bi',read(site+0x1fd30,5))
    assert op==0xe8 and site+5+rel==0xcb5130
print('C-032 PASS')
