#!/usr/bin/env python3
"""A-010 original SELF static evidence verifier. No game bytes embedded."""
import hashlib, pathlib, struct, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
WINDOWS=[
 (0x0D049F0,80,"da21b77be4759a5e47540595de550e22e00a5ac9890d235dbb36400c7a78b795"),
 (0x0D04A83,150,"13922b2025329ab4f83d6f90bcb7fc6df957679c99444061de304360cccf604e"),
 (0x0D04D71,82,"5fb860d74b03de174566afaf6843e20b14f33fd8e2d629a7e095617bfa3b9892"),
]
DELTA=0x1FD30
def main(path):
 p=pathlib.Path(path)
 assert p.stat().st_size==81506767
 with p.open("rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
  for va,n,want in WINDOWS:
   f.seek(va+DELTA);data=f.read(n)
   assert hashlib.sha256(data).hexdigest()==want,(hex(va),"window mismatch")
   print(f"window {va:#x} -> SELF {va+DELTA:#x}, {n} bytes: OK")
  def at(va,n):
   f.seek(va+DELTA);return f.read(n)
  # Call at 0xD04DBE is E8 rel32 to existing attachment context method.
  call=at(0x0D04DBE,5)
  assert call[0]==0xE8
  assert 0x0D04DC3+struct.unpack("<i",call[1:])[0]==0x0CC2420
  # Original instruction at 0xD04D94 loads the global stamp.
  op=at(0x0D04D94,6)
  assert op[:2]==bytes.fromhex("8b05")
  assert 0x0D04D9A+struct.unpack("<i",op[2:])[0]==0x04496748
  # Entry context+0x460 and indexed table pointer are checked by the first window digest.
 print("A-010 PASS: original bytes and call/global references. No runtime reachability asserted.")
if __name__=="__main__":main(sys.argv[1])
