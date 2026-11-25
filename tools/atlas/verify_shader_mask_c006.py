#!/usr/bin/env python3
"""C-006: verify original SELF bytes and disassemble independently, read-only."""
import argparse, hashlib, pathlib, subprocess, tempfile
SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
BASE=0x1fd30
WINDOWS={
 0x3220:(0x70,"01619330eec6eff7b322087c1c8716ddbda134eb084dce073eecd71c318c97c9"),
 0x3450:(0x70,"03f6955fa6a07b57d302da84c9409c2d49e571d0724ff6e948cbbb1f59d8d987"),
 0x240:(0x140,"843119267478bd061e4acc9eb629ec301f91a47e51c80c743275ccdd4524e6f9"),
 0x3091cb6:(0x50,"198dd1c78b3cecbb243c656410aee52ec024427564cbc9036cded2e71d035e73")
}
def main():
 ap=argparse.ArgumentParser();ap.add_argument("self_file",type=pathlib.Path);ap.add_argument("--disassemble",action="store_true");a=ap.parse_args()
 with a.self_file.open("rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==SHA
  blobs={}
  for va,(size,digest) in WINDOWS.items():
   f.seek(va+BASE);b=f.read(size)
   assert hashlib.sha256(b).hexdigest()==digest,(hex(va),"window mismatch")
   blobs[va]=b
 assert bytes.fromhex("be 06 02 00 00 c4 e2 48 f7 0c 8a") in blobs[0x3220]
 assert bytes.fromhex("be 0c 02 00 00 c4 e2 48 f7 0c 8a") in blobs[0x3450]
 assert bytes.fromhex("e8 46 15 f7 fc") in blobs[0x3091cb6]
 assert bytes.fromhex("e8 6a 17 f7 fc") in blobs[0x3091cb6]
 assert bytes.fromhex("c4 42 68 f2 f6") in blobs[0x3091cb6]
 if a.disassemble:
  with tempfile.TemporaryDirectory() as tmp:
   for va,b in blobs.items():
    q=pathlib.Path(tmp)/"window.bin";q.write_bytes(b)
    subprocess.run(["objdump","-D","-b","binary","-m","i386:x86-64","-M","intel",f"--adjust-vma={va}",str(q)],check=True)
 print("C-006 original SELF verified")
if __name__=="__main__":main()
