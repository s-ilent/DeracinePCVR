#!/usr/bin/env python3
"""C-005: read-only original-SELF verification, no copyrighted bytes embedded."""
import argparse,hashlib,pathlib,struct,subprocess,tempfile
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
OFFSET=0x1fd30
def main():
 p=argparse.ArgumentParser();p.add_argument("self_file",type=pathlib.Path);p.add_argument("--disassemble",action="store_true");a=p.parse_args()
 with a.self_file.open("rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
  def read(va,n):
   f.seek(va+OFFSET);b=f.read(n);assert len(b)==n;return b
  classifier=read(0x308f8f0,0x190)
  assert hashlib.sha256(classifier).hexdigest()=="73901cdca9bb7fd28fabc6ee1bdabb24897910c29d55441cf26f60514f175b49"
  helper=read(0xab0,0x10)
  assert helper[:13]==bytes.fromhex("b8 df dc f7 07 48 0f a3 f8 0f 92 c0 c3")
  table=read(0x35af3a0,0xe0)
  assert struct.unpack_from("<4I",table,0xa0)==(0,0x100000,0x140000,0x1c0000)
  assert struct.unpack_from("<4I",table,0xd0)==(0xe00000,0xe00000,0x800000,0x800000)
  caller=read(0x3091bf0,0x140)
  assert caller[0x44:0x49]==bytes.fromhex("e8 b7 dc ff ff")
  assert caller[0x57:0x5c]==bytes.fromhex("e8 64 ee f6 fc")
  if a.disassemble:
   with tempfile.TemporaryDirectory() as td:
    for va,b in [(0x308f8f0,classifier),(0xab0,helper),(0x3091bf0,caller)]:
     q=pathlib.Path(td)/"window";q.write_bytes(b)
     subprocess.run(["objdump","-D","-b","binary","-m","i386:x86-64","-M","intel",f"--adjust-vma={va}",str(q)],check=True)
 print("C-005 original SELF verified")
if __name__=="__main__":main()
