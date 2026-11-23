#!/usr/bin/env python3
"""Read-only C-023 original PS4 SELF evidence verifier."""
import hashlib, pathlib, subprocess, sys
p=pathlib.Path(sys.argv[1]); assert p.stat().st_size==81506767
with p.open("rb") as f:
 assert hashlib.file_digest(f,"sha256").hexdigest()=="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
 f.seek(0xCD8500); data=f.read(0xC5)
assert hashlib.sha256(data).hexdigest()=="c02ccffb4bd519a3249d5dc2ddcec8518e3d2e710d12b15d7e95136fa0f1ec02"
print("C-023 verified SELF + ELF 0xCB87D0..0xCB8895 window PASS; decode with objdump -D -b binary -m i386:x86-64 --adjust-vma=0xCB87D0")
