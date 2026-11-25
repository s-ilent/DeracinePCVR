#!/usr/bin/env python3
"""C-021 read-only original SELF verification; no proprietary bytes embedded."""
import hashlib,sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
REGIONS=[
 (0x0CC0190,0x0CC01C1,"4c341b938c414cbdf6bd51e0a282be7c03b4a7848e95ba2fad9795ece4274549"),
 (0x0CC01F4,0x0CC023C,"4320f0bf3e168fbbbee555920f54558381af01d633cd4a205cbfd797d1685fae"),
 (0x0CC0266,0x0CC02D2,"040fd6d5c502e95c3601df8c291dc2ad388118474988770895b55a523a40b4e1")]
with open(sys.argv[1],"rb") as f:
 assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
 for a,b,h in REGIONS:
  f.seek(a+0x1FD30)
  assert hashlib.sha256(f.read(b-a)).hexdigest()==h,hex(a)
print("PASS C-021: original SELF and three independently decoded stream windows")
