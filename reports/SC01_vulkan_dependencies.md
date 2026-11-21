# SC-01 Phase 4: dependencies for the first native Vulkan draw

Derived from the observed command stream, not from a general roadmap. Each item states whether
the data is already available in the shadPS4 Vulkan backend or needs further PS4 UE4 inspection.

## What the capture already provides

Per draw, from the `DrawIndexedPrimitive` payload:

- `IndexBuffer` — an opaque identity, not contents.
- `PrimitiveType`, `BaseVertexIndex`, `FirstInstance`, `NumVertices`, `StartIndex`,
  `NumPrimitives`, `NumInstances`.

Per pass state, from the other four commands:

- Render targets and depth target as opaque identities, plus counts.
- Viewport rectangle and depth range.
- Per-stream vertex buffer identity, offset, and the stream index.

That is enough to describe *what* to draw and *where*. It is not enough to draw.

## Dependencies, in priority order

### 1. Shader identity and compiled shader access — BLOCKING

**Status: needs further PS4 UE4 inspection.**

The capture does not see shader binding at all. A6/A7 established that it does not go through a
command object: it happens inside the recorder at `0x0CB5130`, reached from context vtable slot
`+0x340`.

What is needed:

- A hook on the recorder, or on `sceGnmSetVsShader` / `sceGnmSetPsShader350` directly.
- The shader resource identity from `shader-state + 0x200`.
- The program data at `shader-resource + 0x1D8`, which holds the GCN program.
- The `shader_modifier` from `shader-state + 0x1C0`, which rewrites `RSRC1`.

The shadPS4 backend already translates GCN programs to SPIR-V, so the *translation* exists. What
is missing is the association between a captured draw and the shader pair that was bound when it
executed. That association is the blocking dependency.

### 2. Vertex and index buffer contents and formats — BLOCKING

**Status: partially available.**

The capture gives buffer identities and offsets. The backend already resolves guest GPU
addresses to host buffers, and `SetStreamSource` computes a vertex count from size and stride.

What is missing:

- The vertex declaration, which maps stream index to attribute format and offset. That is a
  separate RHI resource and was not recovered.
- The index buffer format (16-bit or 32-bit), which the draw payload does not carry.

Both need further inspection. The vertex declaration is the larger gap.

### 3. Textures and samplers — BLOCKING

**Status: needs further PS4 UE4 inspection.**

No texture or sampler command was recovered. The A5 search found five commands and none binds a
texture. Either the binding happens through a command outside the searched cluster, or it is
folded into the shader-resource path.

This is the largest unknown. A draw cannot be issued without knowing which textures the shaders
sample.

### 4. Depth, blend, rasterizer and pipeline state — PARTIAL

**Status: partially available.**

- Depth target identity: captured.
- Viewport: captured.
- Blend state, rasterizer state, depth-stencil state: **not captured**. The public header has
  `FRHICommandSetBlendState`, `FRHICommandSetRasterizerState` and
  `FRHICommandSetDepthStencilState`; none was found in the searched cluster.

The backend already builds Vulkan pipelines from GCN register state, so the *pipeline
construction* exists. What is missing is the semantic state that precedes it.

### 5. Resource lifetime and synchronization — PARTIAL

**Status: known constraint, not a data gap.**

A9 established that the executor resets the command allocator and runs no destructors. So a
capture cannot hold a resource pointer past the walk.

What this means for a native draw:

- Resource identities must be resolved to host objects *during* the walk, or re-resolved later
  through a stable key.
- The backend's existing resource tracking is the right place to do that, not the capture.

Synchronization is already handled by the backend's scheduler. No new work is implied.

## What is already available in the shadPS4 Vulkan backend

These do not need new PS4 inspection:

- GCN program to SPIR-V translation.
- Pipeline cache and pipeline construction from register state.
- Guest address to host buffer/image resolution.
- Command-buffer submission and synchronization.
- Render-target and depth-target creation from descriptors.

So the gap is not the renderer. It is the **semantic association**: which shader, which vertex
declaration, which textures were bound when a given draw executed.

## The single highest-value next step

Instrument the recorder at `0x0CB5130` and record, per invocation, the shader resource identity
and the `shader_modifier`. That would let a captured draw be paired with its shader pair.

Without it, the command stream describes geometry with no material. With it, the stream becomes
a draw description.

That is one hook, on one function, using the same detour already proven on five others.

## What this list deliberately excludes

- Renderer changes. None are implied by the capture.
- Performance work. The capture is within noise and the scene is front-end limited.
- A full Vulkan backend. The dependencies above are the prerequisites for *one* draw, not for a
  renderer.
