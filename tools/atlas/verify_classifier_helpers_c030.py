#!/usr/bin/env python3
"""C-030 read-only original SELF verifier; requires GNU objdump."""
import hashlib, subprocess, tempfile, os, sys
assert len(sys.argv) == 2, "usage: verify_classifier_helpers_c030.py <private-original-SELF>"
expected = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
regions = [
 (0x3220,0x3283,"27d8849207e5c99787adec58cd550efc42ea7ed971f852801f822323b6732feb",["call   0x240","mov    esi,0x206","bextr  ecx,DWORD PTR [rdx+rcx*4],esi","mov    DWORD PTR [rbx],ecx"]),
 (0x3450,0x34b3,"e1b80bbcc9b52947a82ae91c2af274b6a3d8ee1019149b1d3fee14a33941d97a",["call   0x240","mov    esi,0x20c","bextr  ecx,DWORD PTR [rdx+rcx*4],esi","mov    DWORD PTR [rbx],ecx"]),
 (0x308fe70,0x308ff75,"449647116815bce4fcb71bc71429712c1643a3c90cd1a405d679afefc6da45c7",["call   0x3450","call   0x3220","andn   r14d,edx,r14d"])
]
with open(sys.argv[1],"rb") as f:
 assert hashlib.file_digest(f,"sha256").hexdigest() == expected, "wrong original SELF"
 for a,b,sha,anchors in regions:
  f.seek(a+0x1fd30)
  data=f.read(b-a)
  assert hashlib.sha256(data).hexdigest()==sha, f"region mismatch: {a:x}"
  tmp=tempfile.NamedTemporaryFile(delete=False)
  try:
   tmp.write(data);tmp.close()
   out=subprocess.run(["objdump","-D","-b","binary","-m","i386:x86-64","-M","intel",f"--adjust-vma={a}",tmp.name],check=True,capture_output=True,text=True).stdout
  finally: os.unlink(tmp.name)
  for anchor in anchors: assert anchor in out, f"missing {anchor} at {a:x}"
  print(f"PASS ELF 0x{a:X} SELF 0x{a+0x1fd30:X}, {b-a} bytes")
print("C-030 original-target verification PASS")
