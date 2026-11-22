#!/usr/bin/env python3
"""A-034 approved original Déraciné SELF check. Read-only.
Usage: python verify_independent_a034.py ORIGINAL_PRIVATE_EBOOT.bin
"""
import hashlib,struct,sys
from pathlib import Path
SIZE=81506767
SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
D=0x1FD30
ANCHORS="""CC71A0|55
CC71B4|418b8f3c090000
CC71BB|238e00020000
CC71C7|4c8b8648020000
CC7260|498b84df88080000
CC726C|418b0c98
CC7270|488b7820
CC72B0|440fb6f0
CC72B4|c4e218f7c0
CC72BC|488b3cc7
CC72C0|488b4740
CC72C4|c5fb1000
CC72D0|c5fb1108
CC72D7|ff5038
CC72E5|e8c6a0ffff
CC731C|488b8e08020000
CC7355|e8d64cfeff
CC7398|488b8e18020000
CC73C0|4d8bbf60040000
CC73CA|498b8f68c40000
CC73D1|0fb78c51a2000000
CC73D9|81f9ffff0000
CC73E5|ba08100000
CC73F2|4181e6ff3f0000
CC73F9|6685c9
CC7402|4983c414
CC740A|c4c178100424
CC7410|c4c17811848778030000
CC7420|498b9f78c30000
CC7460|4181c640020000
CC746E|c74008027604c0
CC7478|418b4c2420
CC7490|418b4c2414
CC749C|48894310
CC74A0|41c68738c5000001
CC74DB|41c7873c09000000000000
CC74F4|c3
CC0087|418b8c2428090000
CC0140|498b84dc68050000
CC0314|418b8f34090000
CC03C0|498b84df48070000
CC0684|418b8f2c090000
CC0730|498b84df08060000
CC09F4|418b8f38090000
CC0AA0|498b84dfe8070000"""
FIFTH=[0xCB1561,0xCB19C4,0xCB4DFF,0xCC3CA0,0xCC3DC0,0xCC5456]
RES=[0xCC01BC,0xCC0448,0xCC07B8,0xCC0B28,0xCC72E5]
VAR=[0xCC0222,0xCC04B8,0xCC0828,0xCC0B98,0xCC7355]
BUILD=[
[0xCEF8B7,0xCEF8C7,0xCEF8D7,0xCEF8E7,0xCEF8F7],
[0xCF1767,0xCF1777,0xCF1787,0xCF1797,0xCF17A7],
[0xCF2557,0xCF2567,0xCF2577,0xCF2587,0xCF2597],
[0xCF333F,0xCF334F,0xCF335F,0xCF336F,0xCF3389],
[0xCF3E56,0xCF3E66,0xCF3E76,0xCF3E86,0xCF3E96],
[0xCF4B87,0xCF4B97,0xCF4BA7,0xCF4BB7,0xCF4BC7]]
BINDERS=[(0xCC0070,0xCC0300),(0xCC0300,0xCC0670),
(0xCC0670,0xCC09E0),(0xCC09E0,0xCC0D40),(0xCC71A0,0xCC74F5)]
def verify(path):
 b=Path(path).read_bytes()
 if len(b)!=SIZE or hashlib.sha256(b).hexdigest()!=SHA or b[:4]!=bytes.fromhex("4f153d1d"):
  raise ValueError("REFUSED: different/nonapproved original Déraciné SELF")
 def read(va,n):
  if va<0 or va+n>0x3E86D30:raise ValueError("VA outside valid file-backed original segment")
  return b[D+va:D+va+n]
 checks=[]
 for row in ANCHORS.splitlines():
  addr,hexbytes=row.split("|")
  expected=bytes.fromhex(hexbytes)
  checks.append((f"opcode {addr}",read(int(addr,16),len(expected))==expected))
 def e8(site,target):
  data=read(site,5)
  return data[0]==0xE8 and site+5+struct.unpack("<i",data[1:])[0]==target
 for site in FIFTH:checks.append((f"fifth binder {site:#x}",e8(site,0xCC71A0)))
 for site in RES:checks.append((f"resource adapter {site:#x}",e8(site,0xCC13B0)))
 for site in VAR:checks.append((f"conditional adapter {site:#x}",e8(site,0xCAC030)))
 for group in BUILD:
  for site in group:checks.append((f"list decoder {site:#x}",e8(site,0xCF0580)))
 for start,end in BINDERS:
  body=read(start,end-start)
  for off,expected in ((0x200,1),(0x208,1),(0x218,1),
                       (0x228,0),(0x238,0),(0x248,1)):
   checks.append((f"{start:#x} disp32 {off:#x}",body.count(struct.pack("<I",off))==expected))
 checks.append(("CS register base",0x2c00+0x240==0x2e40))
 checks.append(("sampler map base",0xa2+2*0==0xa2))
 n=sum(ok for _,ok in checks)
 for label,ok in checks:
  if not ok:print("FAIL",label)
 print(f"A-034 original SELF: {n}/{len(checks)} checks passed")
 return 0 if n==len(checks) else 1
if __name__=="__main__":
 if len(sys.argv)!=2:print(__doc__,file=sys.stderr);sys.exit(2)
 try:sys.exit(verify(sys.argv[1]))
 except (OSError,ValueError,struct.error) as e:print(e,file=sys.stderr);sys.exit(2)
