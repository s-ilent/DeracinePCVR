#!/usr/bin/env python3
"""Read-only C-004 original SELF digest/window verifier."""
import argparse,hashlib,pathlib,subprocess,tempfile
SELF_SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
WINDOWS=((0x3091bf0,0x140,"ef81479b562cad5aaa26bac08e38f89ad4ea96aa04fe0afbb4d46730164469a0"),(0xcb5867,0xa4,"8e53e6f3515108b0559c3647d6e0c852bd128a4bd45cffc0d54ddad7d2841be8"))
def main():
 p=argparse.ArgumentParser();p.add_argument("self_file",type=pathlib.Path);p.add_argument("--disassemble",action="store_true");a=p.parse_args()
 with a.self_file.open("rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==SELF_SHA
  for va,n,sha in WINDOWS:
   f.seek(va+0x1fd30);b=f.read(n)
   assert len(b)==n and hashlib.sha256(b).hexdigest()==sha
   if va==0x3091bf0:
    assert b[0x10d:0x112]==bytes.fromhex("c4 42 68 f2 f6")
   else:
    assert b[0xb:0x10]==bytes.fromhex("85 c0 be 12 00")
   if a.disassemble:
    with tempfile.TemporaryDirectory() as td:
     q=pathlib.Path(td)/"window";q.write_bytes(b)
     subprocess.run(["objdump","-D","-b","binary","-m","i386:x86-64","-M","intel",f"--adjust-vma={va}",str(q)],check=True)
 print("C-004 verified")
if __name__=="__main__":main()
