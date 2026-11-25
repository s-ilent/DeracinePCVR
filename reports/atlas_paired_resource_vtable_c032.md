# C-032 — Paired-resource operation has its own relocated context slot

Original 1.00 SELF SHA-256 independently matched `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Baseline 4219a2ebf6e1de8e8541ca3ede9c7d2bd6fcbfb4. C-016 previously left the FDE and indirect dispatch unresolved.

The original GNU EH frame header at ELF VA 0x03CFC414 (SELF +0x1FD30) uses encodings 0x1B/0x03/0x3B and contains a sorted entry for 0x0CB4330 pointing to FDE 0x038F73B0. The FDE uses a PC-relative signed initial location and range 0xE00, ending exactly at 0x0CB5130. Its CIE is 0x038F4308. Full method SHA-256 97ed3c973ad8e89089e1a3cdf2439273c388f46f556ebaf9e85422e061bf23d3.

A distinct original data-table slot at ELF VA 0x03EDFFD8 (SELF 0x03F01218; RELRO segment mapping 0x03E88000 -> 0x03EA9240) contains 0x0CB4330. Original SELF RELA record at 0x044BACD0 has r_offset=0x03EDFFD8, r_info=8 (R_X86_64_RELATIVE), addend=0x0CB4330. Adjacent records at +/-24 bytes resolve 0x03EDFFD0 -> 0x0D01160 and 0x03EDFFE0 -> 0x0CB5130. Relative to known context table 0x03EDFCA0, these slots are +0x330/+0x338/+0x340. The paired operation is therefore a separate table entry, not merely a local block of the shader-state method.

Independent objdump from the FDE start confirms the direct call 0x0CB438C -> 0x0CB5130, followed by two input-resource virtual calls +0x38 at 0x0CB4397 and 0x0CB43A3. This joins two adjacent context-table operations via a direct internal call. A whole executable raw E8 scan found no direct caller to 0x0CB4330; that negative is not proof of runtime absence. Slot +0x338 alone cannot identify an indirect caller because unrelated vtables share offsets.

Confidence high for FDE, relocation, table adjacency and direct edge. Exact C++ class, resource vptr implementation, shader stage and runtime reachability remain unresolved. All listed VAs are ELF-domain, not GPU addresses or shader hashes.

Transfer to SC-09/SC-10/SC-11 or FD4: the paired operation can be tested separately from its internal mode-zero state setup; preserve the two resource identities rather than treating the nested call as an independent binding event. Next: resolve input resource vptr provenance for the two +0x38 calls.
