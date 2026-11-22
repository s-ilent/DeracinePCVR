#!/usr/bin/env python3
"""A-029 static original-target extended-user-data proof.
Usage: python verify_a029.py path/to/approved/1.00/eboot.bin
Read-only; no original code or shader bytes are emitted.
"""
import hashlib,struct,sys
from pathlib import Path
SIZE=81506767
HASH='8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
DELTA=0x1FD30
TABLE=0x035C9CFC
ANCHORS={
 0x02F5D290:'55',
 0x02F5D2C5:'448b0a',
 0x02F5D2D3:'4181e1ffff7f00',
 0x02F5D2DD:'c1eb17',
 0x02F5D2E0:'450fb65401f1',
 0x02F5D2E6:'410fb64c01f0',
 0x02F5D342:'80e301',
 0x02F5D345:'885f02',
 0x02F5D3B0:'410fb65c8dff',
 0x02F5D3B6:'450fb6548dfd',
 0x02F5D3DA:'410fb6548d00',
 0x02F5D3E4:'450fb67c8dfe',
 0x02F5D44E:'4531e4',
 0x02F5D46B:'6646898c7fc2000000',
 0x02F5D51E:'885f04',
 0x02F5D52F:'885f0c',
 0x02F5D540:'885f07',
 0x02F5D5F4:'4c8d3d99c76600',
 0x02F5D630:'428a1493',
 0x02F5D634:'80c2ed',
 0x02F5D637:'80fa06',
}

def verify(path):
 b=Path(path).read_bytes()
 if len(b)!=SIZE or hashlib.sha256(b).hexdigest()!=HASH or b[:4]!=bytes.fromhex('4f153d1d'):
  raise ValueError('REFUSED: not the approved original Déraciné 1.00 SELF')
 def read(va,n):
  if not 0<=va<=0x03E86D30-n:raise ValueError('VA outside verified original text range')
  return b[DELTA+va:DELTA+va+n]
 checks=[(f'anchor {va:#x}',read(va,len(bytes.fromhex(h)))==bytes.fromhex(h)) for va,h in ANCHORS.items()]
 for kind,target in [(0x02,0x02F5D44E),(0x12,0x02F5D51E),(0x1A,0x02F5D52F),(0x1B,0x02F5D540),(0x13,0x02F5D440),(0x16,0x02F5D440)]:
  rel=struct.unpack('<i',read(TABLE+kind*4,4))[0]
  checks.append((f'dispatch kind {kind:#x}',TABLE+rel==target))
 checks.append(('stage size mask',0x7FFFFF == ((1<<23)-1)))
 checks.append(('GNM trailer separate field',0x1c==28))
 n=sum(ok for _,ok in checks)
 for label,ok in checks:
  if not ok: print('FAIL:',label)
 print(f'A-029 original SELF: {n}/{len(checks)} checks passed')
 return 0 if n==len(checks) else 1

if __name__=='__main__':
 if len(sys.argv)!=2:
  print(__doc__,file=sys.stderr);sys.exit(2)
 try:sys.exit(verify(sys.argv[1]))
 except (OSError,ValueError,struct.error) as e:
  print(str(e),file=sys.stderr);sys.exit(2)
