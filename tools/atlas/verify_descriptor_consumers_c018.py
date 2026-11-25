#!/usr/bin/env python3
"""C-018 read-only original SELF verifier. No proprietary bytes."""
import hashlib, pathlib, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
BASE=0x1FD30
WINDOWS=((0x02F62A40,0x02F62BCB,"39a67309be19bfccb4380dd3972494f0501eb8837ecd7575b4962176a4c2b301"),(0x02F62BD0,0x02F62D5B,"ca32a701a962d7800b0ac37314e9475c89a65ee1c29584a6c45c2c4a8f006885"))
ANCHORS=((0x02F62AB3,"410fb7444122"),(0x02F62C43,"410fb7844182000000"),(0x02F62AD3,"c5fc1003"),(0x02F62C66,"c5fc1003"),(0x02F62B93,"4883c030"),(0x02F62D23,"4883c030"))
def verify(path):
 with open(path,"rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
  for start,end,digest in WINDOWS:
   f.seek(start+BASE);assert hashlib.sha256(f.read(end-start)).hexdigest()==digest,hex(start)
  for va,hexbytes in ANCHORS:
   f.seek(va+BASE);b=bytes.fromhex(hexbytes);assert f.read(len(b))==b,hex(va)
 print("PASS C-018: SELF digest, two functions and six instruction anchors")
if __name__=="__main__":verify(pathlib.Path(sys.argv[1]))
