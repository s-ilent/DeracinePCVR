# RHI-02: native renderer contract

## Purpose

This is the minimum interface a backend must implement to execute an original Unreal Engine draw
from recovered semantic commands and captured resources. It is derived from the working RHI-02
reference implementation, not designed in advance.

The contract represents what the original UE4 renderer asked for, not what shadPS4 translated the
PS4 command stream into. That distinction is the point of the milestone.

## What the contract is not

- It is not a Vulkan wrapper. The reference backend is Vulkan, but nothing in the command vocabulary
  names a Vulkan type.
- It is not a complete renderer. It covers one indexed draw and the state that precedes it.
- It is not original shader execution. The reference backend runs a diagnostic SPIR-V shader, and
  the contract carries original shader identity separately so the two are never confused.

## Identity domains

These must never be conflated. Each is a distinct namespace with its own provenance.

| Domain | Example | Established by |
| --- | --- | --- |
| Original UE4 RHI object | `FRHIIndexBuffer*` | the RHI invocation argument |
| Guest GPU address | `0x2017431000` | the PS4 resource's guest allocation |
| Capture resource id | `10` | the package's resource table |
| Native GPU handle | `VkBuffer` | the backend, at creation |
| Original GCN program hash | `params.hash` | the guest program header |
| Shader permutation hash | `HashCombine(program, perm)` | shadPS4's pipeline cache |
| Vulkan pipeline | `VkPipeline` | the backend, at creation |

A guest pointer is not a native handle. A GCN program hash is not a permutation hash. A capture
resource id is not a descriptor binding.

## Operation vocabulary

The command stream is a bounded, ordered sequence of typed operations. Each carries an evidence
class so a consumer can tell a verified fact from a tentative attribution.

```
BeginFrame
SetViewport        { min_x, min_y, min_z, max_x, max_y, max_z }
SetRenderTargets   { color_targets[], depth_target }
BindVertexBuffers  { resources[] }
BindIndexBuffer    { resource, element_size }
SetGraphicsPipeline{ pipeline }
BindShaderResources{ resources[] }
DrawIndexed        { payload, derived }
EndFrame
Opaque             { sequence, note }
```

`Opaque` exists so a capture is honest about a command whose semantics are not established, rather
than silently dropping it.

### Evidence classes

| Class | Meaning |
| --- | --- |
| `VerifiedTargetBinary` | Established from the target binary: disassembly, payload offsets, call contract |
| `PublicSourceCorrespondence` | Matches pinned public UE4.18.3 source, not independently confirmed in this build |
| `RuntimeConfirmed` | Observed executing at runtime with the expected arguments |
| `Tentative` | Plausible by shape only; never used to claim semantic completeness |

## The draw payload

The original UE4 `FRHICommandDrawIndexedPrimitive` arguments, verbatim:

```
index_buffer_resource   capture resource id of the bound index buffer
primitive_type          UE4 EPrimitiveType, verbatim
base_vertex_index       signed, verbatim
first_instance          verbatim
num_vertices            verbatim
start_index             verbatim, an index offset not a byte offset
num_primitives          verbatim
num_instances           verbatim
```

### The derivation

One conversion, in one place, tested against the downstream oracle:

```
index_count       = num_primitives * 3        (triangle list)
first_index       = start_index
vertex_offset     = base_vertex_index         (signed)
instance_count    = num_instances
index_byte_offset = start_index * element_size
```

UE4's `PT_TriangleList` is **0**. The AMD GPU topology numbers triangle list as 4. Conflating them is
a real defect that SC-07R established.

A consumer must recompute these from the payload and compare against any stored derived values. A
disagreement is `derived_inconsistent` and the tape is not executable. A backend must never receive
an unverified number.

## Resource model

Resources are described by portable identity, not by native handles.

```
Resource {
    id                    capture resource id
    kind                  IndexBuffer | VertexBuffer | RenderTarget | DepthTarget | ShaderResource
    provenance            Ue4Object | DownstreamOracle | Unresolved
    package_resource_ref  the package entry that carries the bytes
    guest_base            guest GPU address, when known
    guest_size            guest extent, when known
    element_size          index width, when known
}
```

`provenance` is the distinction the milestone exists to make. A resource recovered from a UE4 object
is not the same claim as one recovered through the downstream execution oracle.

### Required backend operations

| Operation | Contract |
| --- | --- |
| Create buffer | size, usage, memory preference; returns a native handle |
| Upload buffer | native handle, bytes, offset |
| Create image | format, extent, mip levels, layers, usage |
| Upload image | native handle, subresource, bytes, row pitch |
| Create image view | image, format, aspect, subresource range |
| Create sampler | filter, address modes, LOD range, anisotropy, compare |
| Bind vertex buffers | binding index, native handle, offset |
| Bind index buffer | native handle, offset, element width |
| Bind sampled resource | binding, view, sampler |
| Set viewport and scissor | rectangle, depth range |
| Issue indexed draw | index count, instance count, first index, vertex offset, first instance |
| Barrier | image or buffer, old and new state, source and destination stage |
| Read back | image, region, destination buffer |

## Shader contract

A backend must eventually consume:

- the effective shader module, which is the patch if patched and the generated SPIR-V otherwise;
- the permutation identity, because a program hash alone does not identify the module;
- the descriptor interface, which classifies each binding;
- required parameters and buffers;
- pipeline state;
- resource bindings.

### Permutation identity

```
program hash      = params.hash
permutation index = the slot in Program::modules
permutation hash  = HashCombine(program_hash, permutation_index)
effective module  = patch SPIR-V if patched, else the generated SPIR-V
```

`HashCombine` is `seed ^ (hash + 0x9e3779b9 + (seed << 12) + (seed >> 4))`.

Seven identities stay separate: original UE4 shader-resource identity, original GCN program
identity, permutation index, permutation hash, generated SPIR-V identity, effective SPIR-V identity,
and Vulkan pipeline identity.

### The current limitation

The contract can express the shader module and its identity. It cannot yet express the descriptor
**set** layout, because that classification is not captured. The reference backend therefore runs a
diagnostic shader and reports the original shader identity separately.

That is a test mode. It is not original shader execution, and a consumer must not present it as such.

## State completeness

Every effective state element carries one of:

| Class | Meaning |
| --- | --- |
| Recovered from original UE4 execution | The value came from the deferred command payload |
| Recovered from original PS4 RHI context | The value came from the RHI context, not a command |
| Observed downstream through shadPS4 | The value came from the translated path |
| Explicitly substituted for diagnostic execution | The value is ours, and labelled |
| Unresolved | The value is not known |

Viewport, pipeline, render targets, depth and blend state all carry this distinction. A substituted
viewport must not silently become recovered state.

## Resource lifetime

Ownership is explicit.

| Resource | Owner | Lifetime |
| --- | --- | --- |
| Captured bytes | the package | the package's lifetime |
| Native buffers and images | the backend | created before the draw, destroyed after readback |
| Render targets | the backend | one diagnostic pass |
| OpenXR composition images | the OpenXR runtime | borrowed, acquire/release |
| Original UE4 objects | the game | valid only between append and the allocator reset |

The last row is why the tape stores scalar payload values rather than pointers. The deferred command
allocator resets wholesale after execution and runs no destructors, so a retained pointer would read
freed memory.

## Synchronization

The backend owns its barriers. The contract requires:

- an image uploaded before it is sampled must transition from transfer-destination to
  shader-read-only, with the barrier outside any render pass;
- a buffer-to-image copy must not be recorded inside a subpass;
- a depth-only image must be viewed through the depth aspect;
- a sampler requesting anisotropy requires the device feature enabled.

These are not style preferences. Each was a validation error in the reference implementation before
it was fixed.

## OpenXR compatibility

The contract permits a native OpenXR runtime to supply the device and the composition images. It
does not require shadPS4 to own:

- the Vulkan instance;
- the physical device;
- the logical device;
- the graphics queue;
- the render targets;
- the OpenXR swapchain images.

A backend adopting an external device receives it at creation and must not destroy it. A backend
rendering into a composition image must acquire it, render, and release it, and must not allocate or
destroy it.

**OpenXR integration has not been demonstrated.** The contract permits it; nothing implements it.

## Reusability

| Component | Classification |
| --- | --- |
| Operation vocabulary | Generic to Unreal's RHI architecture |
| Draw payload and derivation | Specific to the PS4 UE4 implementation |
| Identity model | Generic to UE4's deferred RHI, with PS4-specific fields |
| Resource model | Generic, with PS4-specific provenance values |
| Permutation identity | Generic to shadPS4's shader compilation |
| Diagnostic replay | Specific to the diagnostic replay |

Déraciné, Summer Lesson and Happy Manager are Unreal Engine titles on PS4, so the vocabulary and the
identity model should transfer. The payload layout is UE4.18-specific: a different engine version
could reorder or resize it, and the decoder would need a version-specific path rather than assuming
the layout.

No Déraciné-only identity is hardcoded into the portable vocabulary.
