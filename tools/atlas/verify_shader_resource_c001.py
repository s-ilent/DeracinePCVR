#!/usr/bin/env python3
"""Verify C-001 anchors against a privately supplied original SELF; never outputs game bytes."""
import hashlib, pathlib, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
REGIONS=[
 (0x0CB5130,0x430,"19e0b129ad7016de5b6794339c3abf7c5e50ebb59d47f447ccee237f245138fa"),
 (0x0CB5220,0x158,"e1aabbfc58d7129b07c8a1c8aa4ce0e2c4337ed10b511fa99bf2de9939748f35"),
 (0x0CB53A0,0x150,"274d335ccd1631cb00a76935e96a932df0df72f92688e6a6a5a9f46c0edd6bf5")]
def main(p):
 with open(p,"rb") as f:
  digest=hashlib.file_digest(f,"sha256").hexdigest()
  assert digest==EXPECTED,(digest,"unexpected SELF")
  for va,size,want in REGIONS:
   f.seek(va+0x1FD30);got=hashlib.sha256(f.read(size)).hexdigest()
   assert got==want,(hex(va),got)
   print(f"PASS ELF VA {va:#x} / SELF {va+0x1FD30:#x} ({size} bytes)")
if __name__=="__main__":main(pathlib.Path(sys.argv[1]))
