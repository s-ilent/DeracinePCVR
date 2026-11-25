#!/usr/bin/env python3
"""A-009: verify backing-object metadata writes on original Déraciné SELF."""
import hashlib
import pathlib
import subprocess
import sys

EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
WINDOWS={
  0x0CC7E43:"498b45004c89efff50384989c64c89e7418b763ce8e41300004d85f60f95c0488944243874388a051bc5ae03f6d0a801742c8b05cde87c0341837e0800750841c746080100000041c64610014189460cb001eb05",
  0x0CC7F3C:"486305b9e67c03488d0d7ee37c03488b84c1b80100004883c0604939c60f8551030000",
}
def main(path):
 p=pathlib.Path(path)
 h=hashlib.file_digest(p.open("rb"),"sha256").hexdigest()
 assert p.stat().st_size==81506767 and h==EXPECTED, (p.stat().st_size,h)
 with p.open("rb") as f:
  for va,hexbytes in WINDOWS.items():
   want=bytes.fromhex(hexbytes)
   f.seek(va+0x1fd30)
   got=f.read(len(want))
   assert got==want,(hex(va),got.hex())
   print(f"SELF VA {va:#x} file offset {va+0x1fd30:#x}: {len(want)} bytes verified")
 print("Inspect boundaries: objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0xCC7E43 <window>")
 print("A-009 PASS: original bytes; interpretation remains static-only")
if __name__=="__main__":main(sys.argv[1])
