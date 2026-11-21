# A4 analysis scripts

Small read-only helpers used for the A4 dispatch investigation. They take a plain ELF, so
unwrap the pinned `eboot.bin` first:

```
node tools/ps4elf.mjs unwrap <eboot.bin> <out.elf>
```

The unwrapped ELF keeps the original virtual addresses, so every VA in the A4 report can be
used directly.

## Scripts

`refs.py <elf> <va>` — direct `E8`/`E9` rel32 call sites whose destination is `<va>`, plus any
8-byte literal equal to `<va>` in a file-backed segment. Use this to answer "is this function
called directly, and is its address stored anywhere as data".

`relocs.py <elf> target <va...>` — `R_X86_64_RELATIVE` relocations whose addend is one of the
given VAs. A relocated vtable slot stores its target in the relocation addend, not as a literal
pointer, so `refs.py` alone will miss it.

`relocs.py <elf> slot <va...>` — the relocation (if any) and the raw 8 bytes at each slot.

`rip.py <elf> <va...>` — `lea disp32(%rip), %reg` references to a VA. Table bases are usually
reached this way rather than stored as literals.

## Notes

- The RELRO segment (`PT_SCE_RELRO`, type `0x61000010`) is file-backed but not executable.
  Vtables live there, so any scan that only looks at `PT_LOAD` will miss them.
- These scripts do not disassemble. For instruction-level work use
  `llvm-objdump -d --start-address=... --stop-address=... <elf>`.
