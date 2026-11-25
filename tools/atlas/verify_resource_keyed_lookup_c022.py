#!/usr/bin/env python3
"""Read-only original SELF C-022 window checker; does not extract or publish code."""
import hashlib, pathlib, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
WINDOWS=[(0xCAC030,0xCAC23A,"6b3df5d65d173fc174c489b3c1da0c0baad9327b22e2d6ab36279d07d4dde4c2"),(0xCAC151,0xCAC23A,"6ef67f7b05caac3d1f56db5dc619b6b213c6923ca705adec077691b4e4d75e70"),(0xCAC270,0xCAC336,"cfc1f51027aa3460b12edd65816b9fb04692bc26d2c2fdc30e53f0eb4cd600b0")]
p=pathlib.Path(sys.argv[1]); assert p.stat().st_size==81506767
with p.open("rb") as f:
 assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
 for a,b,expected in WINDOWS:
  f.seek(a+0x1FD30)
  assert hashlib.sha256(f.read(b-a)).hexdigest()==expected,(hex(a),hex(b))
print("C-022 original SELF window hashes PASS; disassemble from 0x0CAC030 to verify instruction semantics")
