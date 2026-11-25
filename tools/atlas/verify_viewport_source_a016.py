#!/usr/bin/env python3
"""Verify A-016 target bytes and decode-critical x86-64 instruction windows."""
import hashlib, pathlib, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
WINDOWS=[(0x1400A40,0x120,"1a68db2ecfffc94953c98dbeb892765e5cd15b9cd905968f24f9feb478ff7398"),(0x13D67E0,0x110,"8da0217846989e6c6bd64350846a89fc6eb0f4006bcd6682d054442bc29f69ad")]
def main(path):
    with open(path,"rb") as f:
        assert f.seek(0,2)==81506767,"SELF size mismatch"
        f.seek(0)
        assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED,"SELF SHA mismatch"
        for va,size,digest in WINDOWS:
            f.seek(va+0x1FD30)
            data=f.read(size)
            assert hashlib.sha256(data).hexdigest()==digest,hex(va)
            print(f"OK ELF VA {va:#x} SELF offset {va+0x1FD30:#x} bytes {size:#x}")
    print("A-016 windows match. Decode with objdump -D -b binary -m i386:x86-64 --adjust-vma=<ELF VA> <extracted window>.")
if __name__=="__main__":
    if len(sys.argv)!=2:raise SystemExit("usage: verify_viewport_source_a016.py <private-original-SELF>")
    main(pathlib.Path(sys.argv[1]))
