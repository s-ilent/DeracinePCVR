#!/usr/bin/env python3
"""Read-only original SELF check for A-014."""
import hashlib,struct,sys
with open(sys.argv[1],"rb") as f:
 assert hashlib.file_digest(f,"sha256").hexdigest()=="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
 def read(o,n):
  f.seek(o);return f.read(n)
 assert struct.unpack("<QQ",read(0x03F26A08+0x21240,16))==(0x13A6BB0,0x13A6BE0)
 assert read(0x13A6BB0+0x1FD30,4)==bytes.fromhex("55 48 89 e5")
 assert read(0x13A6BD3+0x1FD30,5)==bytes.fromhex("e9 08 1f 91 ff")
 assert 0x13A6BD8+struct.unpack("<i",read(0x13A6BD4+0x1FD30,4))[0]==0xCB8AE0
 assert read(0xCB8AE0+0x1FD30,4)==bytes.fromhex("55 48 89 e5")
 assert read(0xCB8BC2+0x1FD30,7)==bytes.fromhex("49 8b 9d 60 04 00 00")
 assert read(0xCB8C1D+0x1FD30,5)==bytes.fromhex("83 f9 0b 77 4b")
print("PASS A-014 original SELF")
