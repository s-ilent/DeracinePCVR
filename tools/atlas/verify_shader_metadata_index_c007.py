#!/usr/bin/env python3
"""C-007: read-only original SELF verification of descriptor-index bounds."""
import hashlib, pathlib, struct, subprocess, tempfile, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
BASE=0x1fd30
WINDOWS={0x240:(0x190,"9f2ed8ee79822114ef5528518f9376d176329b1b7a551973abb21974ad10e89a"),0x2ec0:(0x20,"3c53ec5af6bb522a3c55e654a0eb6939abe6f7d9bf36bf0f0540cf4d04638d61"),0x3020:(0x28,"b7a6cacad8a7830ad5fcb6a4f4f19cb0d27c823f7dda917d7d02a8d1447d1e6e"),0x33ca890:(0xc0,"6e458f4f68f3960f5a0ef7d6385f3a31846f53a0d306a7bc8604f68971165930")}
with pathlib.Path(sys.argv[1]).open("rb") as f:
 assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
 blobs={}
 for va,(n,digest) in WINDOWS.items():
  f.seek(va+BASE); b=f.read(n)
  assert hashlib.sha256(b).hexdigest()==digest,hex(va)
  blobs[va]=b
 assert bytes.fromhex("ba 02 04 00 00 c4 e2 68 f7 04 81") in blobs[0x3020]
 assert bytes.fromhex("f3 0f bd d9 83 f3 1f") in blobs[0x240]
 assert bytes.fromhex("8d 4b 08 84 c0 0f 44 cb") in blobs[0x240]
 table=struct.unpack_from("<16I",blobs[0x33ca890],0x80)
 assert table==(0x26e8,0x26d4,0x21d0,0x21d0,0x2080,0x2040,0x1000,0,0x36ec,0x26e8,0x21d4,0x20d0,0x1080,0x1040,0,0)
 assert all(0<=((x.bit_length()-1)+(8 if flag else 0))<=14 for x in range(1,65) for flag in (0,1))
 if "--disassemble" in sys.argv:
  with tempfile.TemporaryDirectory() as d:
   for va in (0x240,0x2ec0,0x3020):
    q=pathlib.Path(d)/"slice";q.write_bytes(blobs[va])
    subprocess.run(["objdump","-D","-b","binary","-m","i386:x86-64","-M","intel",f"--adjust-vma={va}",str(q)],check=True)
print("C-007 verified original SELF and bounded index")
