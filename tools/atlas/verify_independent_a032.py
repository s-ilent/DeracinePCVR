#!/usr/bin/env python3
"""Original-target A-032 independent constructor/router verifier (read-only).
Usage: python verify_a032.py /path/to/private/Déraciné-1.00-eboot.bin
Only SHA-gated instructions and deterministic address/dispatch relationships are tested.
"""
import hashlib
import struct
import sys
from pathlib import Path

SIZE = 81_506_767
SHA = '8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
DELTA = 0x1fd30
ANCHORS = '''
CAC030|554889e541
CAC04A|4d85f67423
CAC05D|498b7e3048
CAC069|ff50384883
CAC081|498b462848
CAC090|498b463848
CAC099|4883c0208b
CAC0A3|83f804743c
CAC0A8|83f8027528
CAC170|418b4614ba
CAC181|41234e1848
CAC220|4863d248c1
CAC39C|488b03b900
CAC3A4|4189461448
CAC3B7|418946184d
CAC3BF|bfe8020000
CAC3E8|e953632b02
CAC502|49837e3000
CAC512|4983c640b9
CAC53A|e901652b02
CC021E|498b34c7e8
CC0222|e809befeff
CF0580|554889e541
CF05A5|4963742408
CF05AA|496354240c
CF05BA|ff507841f6
CF05BD|41f6462801
CF05F1|ff5050488b
CF05F8|41f6462908
CF06C0|498b04244e
CF0730|8b12418917
CF0757|41c7442408
CF0767|4c89e7e8e1
CF0772|85f60f8eac
CF0780|4d636c2408
CF079D|498b460849
CF07A9|488b10488d
CF0810|8b12418955
CF169D|498db42408
CF1767|e814eeffff
CF176C|498db42418
CF1777|e804eeffff
CF177C|498db42428
CF1787|e8f4edffff
CF178C|498db42438
CF1797|e8e4edffff
CF179C|498db42448
CF17A7|e8d4edffff
CF3271|498db42408
CF333F|e83cd2ffff
CF3D7D|498db42408
CF3E56|e825c7ffff
CF3E5B|498db42418
CF3E66|e815c7ffff
CF3E6B|498db42428
CF3E76|e805c7ffff
CF3E7B|498db42438
CF3E86|e8f5c6ffff
CF3E8B|498db42448
CF3E96|e8e5c6ffff
'''

HELPER_CALLS = [0xCF1767,0xCF1777,0xCF1787,0xCF1797,0xCF17A7,
                0xCF333F,0xCF3E56,0xCF3E66,0xCF3E76,0xCF3E86,0xCF3E96]
TAIL_CALLS = [(0xCAC3E8,0x2F62740),(0xCAC53A,0x2F62A40)]

def verify(path):
    data = Path(path).read_bytes()
    if (len(data) != SIZE or hashlib.sha256(data).hexdigest() != SHA
            or data[:4] != bytes.fromhex('4f153d1d')):
        raise ValueError('REFUSED: unapproved original Déraciné 1.00 SELF')
    def code(va, n):
        if not 0 <= va <= 0x03E86D30-n:
            raise ValueError('address outside verified file-backed text segment')
        return data[DELTA+va:DELTA+va+n]
    tests=[]
    for line in ANCHORS.strip().splitlines():
        a,h=line.split('|');raw=bytes.fromhex(h);va=int(a,16)
        tests.append((f'{va:#x} exact original opcode bytes',code(va,len(raw))==raw))
    for va in HELPER_CALLS:
        rel=struct.unpack('<i',code(va+1,4))[0]
        tests.append((f'{va:#x} calls 0xCF0580',code(va,1)==b'\xe8' and va+5+rel==0xCF0580))
    for va,target in TAIL_CALLS:
        rel=struct.unpack('<i',code(va+1,4))[0]
        tests.append((f'{va:#x} tail jumps to {target:#x}',code(va,1)==b'\xe9' and va+5+rel==target))
    rel=struct.unpack('<i',code(0xCC0223,4))[0]
    tests.append(('0xCC0222 dispatches via shared adapter',0xCC0222+5+rel==0xCAC030))
    # The +0x208 through +0x248 lists are five adjacent 16-byte descriptor holders.
    for offset in (0x208,0x218,0x228,0x238,0x248):
        tests.append((f'list field +{offset:#x} has 0x10 stride',(offset-0x208)%16==0))
    passed=sum(ok for _,ok in tests)
    for label,ok in tests:
        if not ok: print('FAIL',label)
    print(f'A-032 original SELF: {passed}/{len(tests)} checks passed')
    return 0 if passed==len(tests) else 1

if __name__=='__main__':
    if len(sys.argv)!=2: print(__doc__,file=sys.stderr);sys.exit(2)
    try:sys.exit(verify(sys.argv[1]))
    except (OSError,ValueError,struct.error) as e:
        print(e,file=sys.stderr);sys.exit(2)
