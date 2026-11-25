#!/usr/bin/env python3
"""Read-only original SELF verifier for C-028. No proprietary bytes embedded."""
import hashlib, pathlib, struct, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
DELTA=0x1fd30
WINDOWS=[(0xcc27f9,0xcc2843,"2c6164a02562cbfe0d83c170b8a0c70837330af29dd29bcda919ae309452f03b"),(0x308fe70,0x308fefb,"c0610d31c4ed03372167494df22409491448dd46b48789322a5d331b1f86e0b2")]
def run(path):
 with open(path,"rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
  assert f.seek(0,2)==81506767
  for a,b,h in WINDOWS:
   f.seek(a+DELTA); assert hashlib.sha256(f.read(b-a)).hexdigest()==h
  def read(va,n):
   f.seek(va+DELTA);return f.read(n)
  for site,target in [(0xcc2821,0x308fe70),(0xcc2839,0xcc93d0)]:
   x=read(site,5); assert x[0]==0xe8 and site+5+struct.unpack("<i",x[1:])[0]==target
  assert read(0xcc2816,4)==bytes.fromhex("498b7828")
  assert read(0xcc281a,7)==bytes.fromhex("4d8da5d8030000")
  assert read(0xcc2830,4)==bytes.fromhex("48c1e008")
  assert read(0xcc2834,5)==bytes.fromhex("4889442438")
 print("C-028 original SELF windows and call displacements verified")
if __name__=="__main__":run(pathlib.Path(sys.argv[1]))
