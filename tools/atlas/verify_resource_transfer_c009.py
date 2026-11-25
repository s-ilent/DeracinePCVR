#!/usr/bin/env python3
"""C-009 original SELF CPU-side transfer consumers, independent byte verification."""
import hashlib, pathlib, subprocess, sys, tempfile
EXPECTED = '8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
BASE = 0x1fd30
WINDOWS = {
 0x6190:(0x340,'4afd160d0b2d1b841bad5c326ddd51f397f0127c410d88c7380e91c5162370be'),
 0x6f40:(0x340,'4aa6a7c5bfd7c68ca51909b456894596f9864a7c21ae8263efdfbbca2c8e1928'),
 0x5b90:(0x140,'5efea35fbff3a67e1093c507fc82bf03ad375bd3118429f9f357ab8f37a05eb2'),
}
EDGES = [(0x61c3,0x64d0),(0x636c,0x5b90),(0x6380,0x2ec0),
         (0x6f73,0x7280),(0x711c,0x5b90),(0x7130,0x2ec0),(0x5be9,0x4090)]
with pathlib.Path(sys.argv[1]).open('rb') as f:
 assert hashlib.file_digest(f,'sha256').hexdigest()==EXPECTED
 blobs={}
 for va,(length,digest) in WINDOWS.items():
  f.seek(BASE+va); b=f.read(length)
  assert hashlib.sha256(b).hexdigest()==digest,hex(va)
  blobs[va]=b
for site,target in EDGES:
 va=next(v for v,b in blobs.items() if v<=site<v+len(b))
 b=blobs[va];o=site-va
 assert b[o]==0xe8 and site+5+int.from_bytes(b[o+1:o+5],'little',signed=True)==target,(hex(site),hex(target))
# Verify the first wrapper's CPU load/store cases and layout field loads by exact instruction bytes.
for site,sequence in {
 0x61b2:'448b5744', 0x6299:'8b4324',
 0x63db:'0fb60408',0x63df:'42880422',
 0x63f1:'8b0408',0x63f4:'42890422',
 0x6406:'0fb70408',0x640a:'6642890422',
 0x641d:'488b0408',0x6421:'4a890422',
 0x5bbd:'458b7d08',0x5bc1:'418b4538',0x5bc5:'418b4d24',
}.items():
 va=next(v for v,b in blobs.items() if v<=site<v+len(b))
 assert blobs[va][site-va:site-va+len(bytes.fromhex(sequence))]==bytes.fromhex(sequence),hex(site)
if '--disassemble' in sys.argv:
 with tempfile.TemporaryDirectory() as tmp:
  for va,b in blobs.items():
   p=pathlib.Path(tmp)/f'{va:x}.bin';p.write_bytes(b)
   subprocess.run(['objdump','-D','-b','binary','-m','i386:x86-64','-M','intel',f'--adjust-vma={va}',str(p)],check=True)
print('C-009 SELF digest, three windows, seven direct edges and CPU transfer instructions verified')
