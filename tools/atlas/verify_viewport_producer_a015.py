#!/usr/bin/env python3
"""A-015 original SELF viewport producer, read-only assertions."""
import hashlib,struct,sys
p=sys.argv[1]
with open(p,"rb") as f:
 assert hashlib.file_digest(f,"sha256").hexdigest()=="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
 def code(va,n):
  f.seek(va+0x1FD30); return f.read(n)
 def relro(va,n):
  f.seek(va+0x21240); return f.read(n)
 assert struct.unpack("<QQ",relro(0x3F26A08,16))==(0x13A6BB0,0x13A6BE0)
 checks={
  0x1400A9D:"4d8b26",
  0x1400AB3:"49c1ee20",
  0x1400AB7:"49c1ef20",
  0x1400ACF:"498b4530",
  0x1400AD3:"4883c007",
  0x1400AD7:"4883e0f8",
  0x1400ADB:"488d4828",
  0x1400ADF:"493b4d38",
  0x1400B12:"49894d30",
  0x1400B16:"41ff4514",
  0x1400B29:"498b4d08",
  0x1400B2D:"488901",
  0x1400B30:"49897d08",
  0x1400B34:"48c7400800000000",
  0x1400B3C:"488930",
  0x1400B3F:"44896010",
  0x1400B43:"44897014",
  0x1400B47:"c7401800000000",
  0x1400B4E:"89501c",
  0x1400B51:"44897820",
  0x1400B55:"c740240000803f",
 }
 for va,h in checks.items():
  expected=bytes.fromhex(h); assert code(va,len(expected))==expected,hex(va)
 lea=code(0x1400B1E,7)
 assert lea[:3]==bytes.fromhex("488d35")
 assert 0x1400B25+struct.unpack("<i",lea[3:])[0]==0x3F26A08
 assert code(0x13A6BD3,5)==bytes.fromhex("e9081f91ff")
print("PASS A-015: original SELF viewport command construction")
