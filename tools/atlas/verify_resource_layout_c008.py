#!/usr/bin/env python3
"""C-008 original SELF verification: layout producer and three consumers."""
import hashlib, pathlib, subprocess, sys, tempfile
EXPECTED='8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
BASE=0x1fd30
WINDOWS={
 0x5760:(0x360,'d357d8abda4311c9257b796ba7f59e10e198b1099e888d97532abeed77494fc6'),
 0x9274:(0x75,'c30c3b7f01c55b02d23b32956bf17e9646c2d0d726e6cf7f164126a112cc444a'),
 0x95e4:(0x76,'2303236efac21fc7fd139078ed8bc4e1401c56b444aef06b92e5bb8f382656bd'),
 0x994a:(0x86,'68daf4bb2da19a77e7c647c2cd886c7213b6b24c5d33b28432dc148b2e296863'),
 0x3050:(0x28,'adfa2dfb31e18e1af958031c00dc944199765c7758ff26450a21510d6252eb57'),
 0x3340:(0x28,'393185c5dc62abc5848ff29d4357dbeaa8310b4198392bfbf01b8b3f6c73612d')}
with pathlib.Path(sys.argv[1]).open('rb') as f:
 assert hashlib.file_digest(f,'sha256').hexdigest()==EXPECTED
 blobs={}
 for va,(size,digest) in WINDOWS.items():
  f.seek(va+BASE);b=f.read(size)
  assert hashlib.sha256(b).hexdigest()==digest,hex(va)
  blobs[va]=b
for site,target in [(0x92ca,0x5760),(0x963a,0x5760),(0x999a,0x5760),(0x5908,0x34c0),(0x5919,0x3050),(0x593f,0x3290)]:
 window=next((v for v in blobs if v<=site<v+len(blobs[v])),None)
 assert window is not None,hex(site)
 b=blobs[window];off=site-window
 assert b[off]==0xe8
 assert site+5+int.from_bytes(b[off+1:off+5],'little',signed=True)==target
if '--disassemble' in sys.argv:
 with tempfile.TemporaryDirectory() as d:
  for va in blobs:
   p=pathlib.Path(d)/'slice';p.write_bytes(blobs[va])
   subprocess.run(['objdump','-D','-b','binary','-m','i386:x86-64','-M','intel',f'--adjust-vma={va}',str(p)],check=True)
print('C-008 original SELF digest, windows, and six call edges verified')
