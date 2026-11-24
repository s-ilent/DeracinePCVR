# Native RHI view aspects — 2026-11-24

Scope: NRI Vulkan descriptor creation, independent of emulator graphics. Foundation HEAD 6b359f520b9f41f854b2aac41bb74526f6c60e87; main e2f981c40cba7c8490177c20c179cb3608cc1442 verified before branching.

## Observations
Pinned [DescriptorVK.hpp:109–174](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DescriptorVK.hpp#L109-L174): remaining mip/layer counts are expanded against texture metadata (114–116). View usage is role-specific (122–128). Depth/stencil attachments and subpass inputs with undefined layout select all format aspects, while other views pass requested planes (130–142). Native image view creation uses that range (144–157). Stored metadata retains the resulting aspect mask and expected layout, plus whether layout was explicit (159–172). Expected layout is not evidence of an executed transition.

## Proposed host contract
Keep requested plane selection, effective native view aspects, view role, and per-aspect access intent separate. A depth-only sampled view of a combined depth/stencil texture cannot be assumed equivalent to a depth/stencil attachment view, even with identical mip/layer range. Do not infer writable stencil from attachment aspect coverage alone. Cache views by generation, role, format, normalized range, planes, layout and swizzle (conservative key); track actual transitions separately. This is a host design proposal, not recovered UE4 ABI.

## Falsifiable experiment
Create a supported combined depth/stencil image, create depth-only and stencil-only sampled views and an attachment view, inspect native creation parameters and validation output. Test depth-only writes with stencil preservation and independently verify stencil readback. Repeat with explicit and default subpass-input layout if supported. No GPU test executed in this pass.

## Retrieval
Initial web branch retrieval: Failed to fetch restricted URL; public implementation: (503) Service Unavailable. Authenticated GitHub retrieval succeeded for both branch heads and pinned source. No game assets, executable changes, emulator backend work or performance claims.
