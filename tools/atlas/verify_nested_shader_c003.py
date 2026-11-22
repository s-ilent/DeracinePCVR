#!/usr/bin/env python3
"""Read-only original SELF C-003 window verifier; no proprietary bytes embedded."""
import argparse, hashlib, subprocess, tempfile, pathlib
EXPECTED_SELF = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
VA = 0x0CB57CF
SIZE = 0x200
EXPECTED_WINDOW = "a2162445681946519c8d2a46e05622bf8e62dd34c54996f066175fa1028f17e3"
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("self_file",type=pathlib.Path)
    ap.add_argument("--disassemble",action="store_true")
    args=ap.parse_args()
    with args.self_file.open("rb") as f:
        digest=hashlib.file_digest(f,"sha256").hexdigest()
        if digest!=EXPECTED_SELF: raise SystemExit("SELF digest mismatch: "+digest)
        f.seek(VA+0x1FD30)
        data=f.read(SIZE)
    assert len(data)==SIZE
    assert hashlib.sha256(data).hexdigest()==EXPECTED_WINDOW
    # Instruction-boundary anchors in independently decoded window.
    assert data[0x9e:0xa3]==bytes.fromhex("e8 79 c3 3d 02") # 0xCB586D -> 0x3091BF0
    assert data[0xe5:0xea]==bytes.fromhex("e8 37 c3 3d 02") # 0xCB58B4 -> 0x3091BF0
    print("C-003 original SELF and window SHA-256 verified")
    if args.disassemble:
        with tempfile.TemporaryDirectory() as td:
            p=pathlib.Path(td)/"window.bin";p.write_bytes(data)
            subprocess.run(["objdump","-D","-b","binary","-m","i386:x86-64","-M","intel",f"--adjust-vma={VA}",str(p)],check=True)
if __name__=="__main__": main()
