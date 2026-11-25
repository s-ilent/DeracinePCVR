#!/usr/bin/env python3
"""C-016 original SELF read-only evidence verifier; contains no proprietary byte dump."""
import hashlib, pathlib, struct, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
BASE=0x1FD30
WINDOWS=((0x0CB4330,0x0CB4460,"1bda546a3d09a8ce90ca62409e0a79bd424983d2f88cdecabec7caa54a8805c0"),(0x0CB4374,0x0CB43DF,"66a1359a1ff64614ef0f67cd8a99c1b1fc9fb97b0ba701b19994c5de9c138c90"))
def main(path):
 with open(path,"rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
  for s,e,d in WINDOWS:
   f.seek(s+BASE);assert hashlib.sha256(f.read(e-s)).hexdigest()==d
  for site,target in ((0x0CB438C,0x0CB5130),(0x0CB43B4,0x03092D20),(0x0CB43C4,0x03092D20)):
   f.seek(site+BASE);b=f.read(5);assert b[0]==0xE8 and site+5+struct.unpack("<i",b[1:])[0]==target
  for va,opcode in ((0x0CB4397,bytes.fromhex("ff5038")),(0x0CB43A3,bytes.fromhex("ff5038")),(0x0CB444A,bytes.fromhex("48c70001000000"))):
   f.seek(va+BASE);assert f.read(len(opcode))==opcode
 print("C-016 original SELF SHA, windows, calls and virtual-slot/write anchors: PASS")
if __name__=="__main__":main(pathlib.Path(sys.argv[1]))
