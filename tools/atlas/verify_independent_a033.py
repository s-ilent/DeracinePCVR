#!/usr/bin/env python3
"""A-033 source-authenticated shader-list lifetime/binding-mask verifier.
Usage: python A033_verify_group_mask_and_list_lifetime.py PRIVATE_1.00_EBOOT.bin
No game bytes/shaders are exported. All VAs are inner ELF, not host addresses.
"""
from pathlib import Path
import hashlib, struct, sys
SIZE=81506767
SHA='8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
DELTA=0x1FD30
ANCHORS="""CF490F|bf60020000e8370e03
CF4914|e8370e03004989c4c5
CF49F4|41c784240002000000
CF4A00|c4c17c118424390200
CF4A0A|c4c17c118424280200
CF4A14|c4c17c118424080200
CF4B04|8b0041898424000200
CF4B06|418984240002000048
CF4B13|488b8500ffffff8b00
CF4B24|418984240002000044
CF4B2F|f6c408744131c031c9
CF4B40|410fb6940403020000
CF4B49|410fb69c0c00020000
CF4B52|41889c040302000041
CF4B5A|4188940c0002000048
CF4B87|e8f4b9ffff498db424
CF4B8C|498db4241802000048
CF4B97|e8e4b9ffff498db424
CF4B9C|498db4242802000048
CF4BA7|e8d4b9ffff498db424
CF4BAC|498db4243802000048
CF4BB7|e8c4b9ffff498db424
CF4BBC|498db4244802000048
CF4BC7|e8b4b9ffff4c8bbd00
CF16EE|8b0041898424000200
CF16F0|418984240002000048
CF170B|418984240002000044
CF1716|f6c408743a31c031c9
CF1720|410fb6940403020000
CF1729|410fb69c0c00020000
CF1732|41889c040302000041
CF173A|4188940c0002000048
CF1767|e814eeffff498db424
CF176C|498db4241802000048
CF1777|e804eeffff498db424
CF177C|498db4242802000048
CF1787|e8f4edffff498db424
CF178C|498db4243802000048
CF1797|e8e4edffff498db424
CF179C|498db4244802000048
CF17A7|e8d4edffff4d8b7d00
CEB330|554889e5534883ec28
CEB38A|488bbb48020000e86a
CEB391|e86a97fbff488bbb38
CEB396|488bbb38020000e85e
CEB39D|e85e97fbff488bbb28
CEB3A2|488bbb28020000e852
CEB3A9|e85297fbff488bbb18
CEB3AE|488bbb18020000e846
CEB3B5|e84697fbff488bbb08
CEB3BA|488bbb08020000e83a
CEB3C1|e83a97fbff488bbbc8
CEB44A|488bbb48020000e8aa
CEB456|488bbb38020000e89e
CEB462|488bbb28020000e892
CEB46E|488bbb18020000e886
CEB47A|488bbb08020000e87a
CA4B00|554889e54156534889
CA4B0A|4885db0f84d7000000
CA4B91|488b074889de488b40
CC0087|418b8c242809000048
CC0093|238e000200000f8442
CC00B1|4c8b8048020000498d
CC01F8|488b88080200008b14
CC026F|488b89180200008b14
CC0314|418b8f34090000238e
CC031B|238e000200000f8424
CC0327|4c8b864802000041be
CC047F|488b8e080200008b14
CC04F8|488b8e180200008945
CC0684|418b8f2c090000238e
CC068B|238e000200000f8424
CC0697|4c8b864802000041be
CC07EF|488b8e080200008b14
CC0868|488b8e180200008945
CC09F4|418b8f38090000238e
CC09FB|238e000200000f8424
CC0A07|4c8b864802000041be
CC0B5F|488b8e080200008b14
CC0BD8|488b8e180200008945"""
CONSTRUCTOR_CALLS=[0xCF1767,0xCF1777,0xCF1787,0xCF1797,0xCF17A7,
                   0xCF4B87,0xCF4B97,0xCF4BA7,0xCF4BB7,0xCF4BC7]
DESTRUCTOR_CALLS=[0xCEB391,0xCEB39D,0xCEB3A9,0xCEB3B5,0xCEB3C1,
                  0xCEB451,0xCEB45D,0xCEB469,0xCEB475,0xCEB481]
HELPERS=[(0xCC0070,0xCC0300),(0xCC0300,0xCC0670),
         (0xCC0670,0xCC09E0),(0xCC09E0,0xCC0D40)]

def verify(path):
    b=Path(path).read_bytes()
    if len(b)!=SIZE or hashlib.sha256(b).hexdigest()!=SHA or b[:4]!=bytes.fromhex('4f153d1d'):
        raise ValueError('REFUSED: not the approved original Déraciné 1.00 SELF')
    def code(va,n):
        if not (0<=va and va+n<=0x03E86D30):
            raise ValueError('VA outside file-backed executable range')
        return b[va+DELTA:va+DELTA+n]
    checks=[]
    for line in ANCHORS.splitlines():
        address,hexbytes=line.split('|')
        a=int(address,16); e=bytes.fromhex(hexbytes)
        checks.append((f'original instruction anchor {a:#x}',code(a,len(e))==e))
    for va in CONSTRUCTOR_CALLS:
        rel=struct.unpack('<i',code(va+1,4))[0]
        checks.append((f'{va:#x} builds list via 0xCF0580',
                       code(va,1)==b'\xe8' and va+5+rel==0xCF0580))
    for va in DESTRUCTOR_CALLS:
        rel=struct.unpack('<i',code(va+1,4))[0]
        checks.append((f'{va:#x} releases list via 0xCA4B00',
                       code(va,1)==b'\xe8' and va+5+rel==0xCA4B00))
    # Bounded scan of EXACTLY the four A-031 group helpers. This demonstrates
    # direct disp32 usage, not absence of all indirect/unseen consumers.
    for start,end in HELPERS:
        window=code(start,end-start)
        for offset,expected in [(0x208,1),(0x218,1),(0x228,0),(0x238,0),(0x248,1)]:
            actual=window.count(struct.pack('<I',offset))
            checks.append((f'{start:#x}..{end:#x} disp32 +{offset:#x} count={expected}',actual==expected))
    n=sum(ok for _,ok in checks)
    for label,ok in checks:
        if not ok:print('FAIL:',label)
    print(f'A-033 original SELF: {n}/{len(checks)} checks passed '
          f'({len(ANCHORS.splitlines())} opcode anchors, '
          f'{len(CONSTRUCTOR_CALLS)+len(DESTRUCTOR_CALLS)} direct-call edges, '
          f'{len(HELPERS)*5} bounded displacements)')
    return int(n!=len(checks))

if __name__=='__main__':
    if len(sys.argv)!=2:print(__doc__,file=sys.stderr);sys.exit(2)
    try:sys.exit(verify(sys.argv[1]))
    except (OSError,ValueError,struct.error) as exc:
        print(str(exc),file=sys.stderr);sys.exit(2)
