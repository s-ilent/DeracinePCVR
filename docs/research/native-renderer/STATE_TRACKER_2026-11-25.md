# Sparse state assignment — 2026-11-25

Foundation base: 6b359f520b9f41f854b2aac41bb74526f6c60e87; main observed e2f981c40cba7c8490177c20c179cb3608cc1442.

Pinned source: [NRI CommandBufferVK.hpp lines 52–110](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L52-L110). Input-attachment normalization merges matching image ranges when their union can be represented by a rectangle under its rules, repeating until stable. This is not a general resource state tracker.

Independent CPU prototype used explicit cell sets to subtract old coverage, insert new state, and coalesce equal-state ranges only if their union is exactly rectangular. Compared against a dense dictionary after every assignment; also asserted disjointness and no remaining pairwise merge candidates. Seed 418, three aspects, three mips, two layers, four abstract states, 200 sequences of 50 updates. All 10,000 assignments and 200 full resets passed; peak 14 sparse ranges. This is a correctness prototype, not an efficient implementation or proof of globally minimal partitioning. No GPU or game tests.

Proposed contract: track per-resource aspect/mip/layer state; merge only identical full semantic states, not merely layouts. Next: replace cell enumeration with interval arithmetic, compare emitted old/new transition ranges against dense oracle, including same-layout write hazards. Keep native RHI separate from emulator graphics and non-rendering runtime.
