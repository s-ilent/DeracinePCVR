#!/usr/bin/env python3
"""C-017: read-only original SELF SHA/window/direct-call verifier; no proprietary bytes."""
import hashlib, pathlib, struct, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
BASE=0x1FD30
WINDOWS=((0x0CB4AF3,0x0CB4C6B,"612b3d61d99cb34b0a50b50e2655ec0bf64e4782c165dd0c749fb8ef56772081"),(0x0CB4B21,0x0CB4B88,"e073d3988bc6021f6cfb0abcae6055ab0e18f575634991a30d96c7153ebf8868"),(0x0CB4C1F,0x0CB4C6B,"c5495fcfe5956b22f0bf2f1eb105f2cd0d2806e4a20753981d04ef9fabe0cb92"))
CALLS=((0x0CB4B09,0x013B8490),(0x0CB4B1C,0x0CC7500),(0x0CB4C44,0x02F62A40),(0x0CB4C66,0x02F62BD0))
def verify(path):
 with open(path,"rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
  for start,end,digest in WINDOWS:
   f.seek(start+BASE); assert hashlib.sha256(f.read(end-start)).hexdigest()==digest,hex(start)
  for site,target in CALLS:
   f.seek(site+BASE); b=f.read(5)
   assert b[0]==0xE8 and site+5+struct.unpack("<i",b[1:])[0]==target,hex(site)
  f.seek(0x0CB4B30+BASE); assert f.read(7)==bytes.fromhex("498b9548040000")
  f.seek(0x0CB4B63+BASE); assert f.read(7)==bytes.fromhex("48398388c30000")
 print("PASS C-017 original SELF, windows, four direct calls and field anchors")
if __name__=="__main__": verify(pathlib.Path(sys.argv[1]))
