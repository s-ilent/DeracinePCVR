#!/usr/bin/env python3
"""Read-only A-030 original Déraciné 1.00 SELF verifier.
Usage: python verify_independent_a030.py PRIVATE_ORIGINAL_EBOOT.bin
"""
import hashlib,struct,sys
from pathlib import Path

SIZE=81506767
SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
DELTA=0x1fd30
END=0x33c9000

# label, function start, map load, map opcode, header, vector load, vector store,
# packet capacity, reserve call, cursor advance, dirty marker, payload DWORDs
WRITERS=[
 ("cbuf16",0x2f62440,0x2f624b2,"410fb78441c2000000",0x2f6249f,0x2f624d5,0x2f624de,0x2f62508,0x2f62512,0x2f6257f,0x2f625a3,4),
 ("vbuf16",0x2f625c0,0x2f62632,"410fb78441ea000000",0x2f6261f,0x2f62655,0x2f6265e,0x2f62688,0x2f62692,0x2f626ff,0x2f62723,4),
 ("resource16",0x2f62740,0x2f627b2,"410fb7444122",0x2f6279f,0x2f627d2,0x2f627db,0x2f62808,0x2f62812,0x2f6287f,0x2f628a3,4),
 ("rw16",0x2f628c0,0x2f62932,"410fb7844182000000",0x2f6291f,0x2f62955,0x2f6295e,0x2f62988,0x2f62992,0x2f629ff,0x2f62a23,4),
 ("resource32",0x2f62a40,0x2f62ab3,"410fb7444122",0x2f62aa0,0x2f62ad3,0x2f62adb,0x2f62b08,0x2f62b12,0x2f62b93,0x2f62bb4,8),
 ("rw32",0x2f62bd0,0x2f62c43,"410fb7844182000000",0x2f62c30,0x2f62c66,0x2f62c6e,0x2f62c98,0x2f62ca2,0x2f62d23,0x2f62d44,8),
 ("sampler16",0x2f62d60,0x2f62dd2,"410fb78441a2000000",0x2f62dbf,0x2f62df5,0x2f62dfe,0x2f62e28,0x2f62e32,0x2f62e9f,0x2f62ec3,4),
]
CALLS={
 0x2f62440:[0xcab29d,0xcace0a,0xcb9fd6,0xcc0dc7],
 0x2f625c0:[0xcbff98,0xcc16af],
 0x2f62a40:[0xcb4c44],
 0x2f62bd0:[0xcb4c66,0xcc3538,0xcc3b6c,0xcc4917,0xcfef63,0xcff56b,0xd0013b],
 0x2f62d60:[0xcc02b3]
}
DECODER={
 0x2f5d2af:"4531c0",
 0x2f5d3ed:"80e202",
 0x2f5d3f7:"83f602",
 0x2f5d3fa:"c1e60d",
 0x2f5d408:"81ce00800000",
 0x2f5d413:"e976010000",
 0x2f5d424:"0d00800000",
 0x2f5d435:"6646898c7fa2000000",
 0x2f5d45a:"0d00800000",
 0x2f5d46b:"6646898c7fc2000000",
 0x2f5d48e:"0d00800000",
 0x2f5d49f:"6646898c7fea000000",
 0x2f5d4bf:"81ce00800000",
 0x2f5d594:"664289547f22",
 0x2f5d5b8:"664289947f82000000",
}
EXAMPLES=[
 (0,0,3,0x8000),(1,8,0,0x8008),(0,16,3,0),
 (1,24,0,8),(2,28,0,12),(2,32,0,16),(2,4,0,0x8004),(2,8,0,0x8008)
]
def encode(start):
    return 0x8000|start if start<16 else start-16
def encode_typed(kind,start,flags):
    if kind not in range(5):raise ValueError("invalid type")
    return encode(start)|((((flags&2)^2)<<13) if kind in (0,4) else 0)
def run(path):
    b=Path(path).read_bytes()
    if len(b)!=SIZE or hashlib.sha256(b).hexdigest()!=SHA or b[:4]!=bytes.fromhex("4f153d1d"):
        raise ValueError("REFUSED: wrong original Déraciné 1.00 SELF")
    def read(va,n):
        if va<0 or va+n>END:raise ValueError("VA outside approved text mapping")
        return b[va+DELTA:va+DELTA+n]
    tests=[]
    def ck(k,yes):tests.append((k,bool(yes)))
    for label,start,mp,hx,hdr,load,store,cap,req,adv,dirty,dw in WRITERS:
        ck(label+" entry",read(start,4)==bytes.fromhex("554889e5"))
        ck(label+" map",read(mp,len(bytes.fromhex(hx)))==bytes.fromhex(hx))
        ck(label+" sentinel",bytes.fromhex("3dffff0000") in read(mp,20))
        ck(label+" masked offset",bytes.fromhex("4181e5ff3f0000") in read(mp,35))
        ck(label+" sign test",bytes.fromhex("6685c0") in read(mp,45))
        def eq(va,h):return read(va,len(bytes.fromhex(h)))==bytes.fromhex(h)
        ck(label+" packet opcode",eq(hdr,"8d8400007604c0" if dw==4 else "8d8400007608c0"))
        ck(label+" vector load",eq(load,"c4c1781006" if dw==4 else "c5fc1003"))
        ck(label+" vector store",eq(store,"c4c178110480" if dw==4 else "c4c17c110480"))
        ck(label+" packet capacity",eq(cap,"83f907" if dw==4 else "83f90b"))
        ck(label+" packet reserve",eq(req,"be08000000" if dw==4 else "be0c000000"))
        ck(label+" packet advance",eq(adv,"4883c020" if dw==4 else "4883c030"))
        ck(label+" stage dirty",eq(dirty,"c6840750c2000001"))
    for dst,sites in CALLS.items():
        for site in sites:
            disp=struct.unpack("<i",read(site+1,4))[0]
            ck(f"call {site:#x}->{dst:#x}",read(site,1)==b"\xe8" and site+5+disp==dst)
    for va,h in DECODER.items():ck(f"decoder opcode {va:#x}",read(va,len(bytes.fromhex(h)))==bytes.fromhex(h))
    for kind,start,flags,want in EXAMPLES:
        ck(f"real A028R declaration kind={kind} start={start}",encode_typed(kind,start,flags)==want)
    for label,start,index,direct in [
        ("PS CB0",28,12,False),("PS CB1",32,16,False),
        ("VS CB0",4,4,True),("VS CB1",8,8,True)]:
        word=encode(start)
        ck(label+" index",(word&0x3fff)==index)
        ck(label+" direct",bool(word&0x8000)==direct)
    ck("direct zero",encode(0)==0x8000)
    ck("shadow zero",encode(16)==0)
    ck("no encoded start is 0xffff",all(encode(i)!=0xffff for i in range(256)))
    ck("resource class bit 14",encode_typed(0,0,0)==0xc000)
    ck("bit 14 stripped by target index",encode_typed(0,0,0)&0x3fff==0)
    ck("RW class bit 14",encode_typed(4,16,0)==0x4000)
    failed=[name for name,ok in tests if not ok]
    for name in failed:print("FAIL",name)
    print(f"A-030 typed writers: {len(tests)-len(failed)}/{len(tests)} checks passed")
    return int(bool(failed))
if __name__=="__main__":
    if len(sys.argv)!=2:print(__doc__,file=sys.stderr);sys.exit(2)
    try:sys.exit(run(sys.argv[1]))
    except (OSError,ValueError,struct.error) as e:print(str(e),file=sys.stderr);sys.exit(2)
