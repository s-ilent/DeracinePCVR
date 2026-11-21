# SC-02 Phase 5: first Vulkan-draw handoff

One actual indexed draw, described semantically, with the precise next dependency.

## The draw

From `_out/SC02_draw_shader/capture-on-run-4`, sequence 33.

| Field | Value |
| --- | --- |
| Command | `DrawIndexedPrimitive` (vtable RVA `0x03F26A88`) |
| Command object | opaque, not retained |
| Command list | `0x1006AA0A00` |
| RHI context | `0x1004650020` |
| Native thread | 30764 |
| Guest thread | 36 |
| Sequence | 33 |

### Primitive parameters

| Field | Value |
| --- | --- |
| `IndexBuffer` | `0x10044E49E8` (opaque identity) |
| `PrimitiveType` | 0 |
| `BaseVertexIndex` | 0 |
| `FirstInstance` | 0 |
| `NumVertices` | 3 |
| `StartIndex` | 6 |
| `NumPrimitives` | 1 |
| `NumInstances` | 1 |

`NumPrimitives = 1` with `NumVertices = 3` is a single triangle. `PrimitiveType = 0` is the
public `PT_TriangleList`.

### Shader association

| Field | Value |
| --- | --- |
| Shader state | `0x1006C11F00` |
| Shader resource | `0x100509AD50` |
| Program data | `0x100440D434` |
| `shader_modifier` | `0x3000080` |
| Resolution path | alternate slot (`shader-state + 0x1E0`) |

This is the **vertex** shader. The pixel shader is not identified by this capture.

### Render targets and depth

The draw record carries `rt_count=0` and `depth=0x0`. That is not a capture failure: the
render-target binding happened in an earlier `SetRenderTargets` command on the same list, and
the draw command does not repeat it. A consumer must track the last `SetRenderTargets` per list.

The most recent `SetRenderTargets` before this draw is at sequence 4, with `count=1`,
`RT[0]=0x0`, `depth=0x803F2E0C0`.

### Vertex buffers

Not carried by the draw command. The stream sources are separate `SetStreamSource` commands.
This capture recorded 313 of them, so they are available, but pairing them to a draw requires
tracking per-list state.

## What is known versus unresolved

### Known

- The draw parameters, exactly.
- The vertex shader resource, program data, and modifier.
- The command list and context identity.
- The execution thread, both native and guest.
- The render-target and depth identities, via the preceding command.

### Unresolved

| Item | Why |
| --- | --- |
| Pixel shader identity | set through a different path; not captured |
| Vertex declaration | a separate RHI resource; not recovered |
| Index buffer format | not carried by the draw payload |
| Textures and samplers | no binding command recovered at all |
| Blend, rasterizer, depth-stencil state | no command found in the searched cluster |
| Shader hash | the association is a pointer, not a hash |

## The precise next dependency

**The pixel shader identity for a draw.**

The vertex shader is resolved. The pixel shader is not, and a draw cannot be rendered without
both. The pixel-shader path is visible in the receiver:

```
ccd0b9: be 28 00 00 00        movl   $0x28, %esi         ; size = 40
ccd0be: 31 d2                 xorl   %edx, %edx          ; register array = null
ccd0c0: e8 7b b2 6f 02        callq  0x033C8340          ; sceGnmSetPsShader350
```

That call passes a null array, which the shadPS4 handler treats as the null-shader branch: it
clears `SPI_SHADER_PGM_LO_PS`/`HI_PS` to zero. So on this path the pixel shader is either
genuinely unbound, or it is bound by a `SetShReg` sequence that precedes this call and is not
visible as a separate command.

Resolving which requires one targeted observation at `0x0CCD0C0`, recording whether the
preceding register writes carried a non-zero program address. That is the same detour already
proven on six functions.

## What this does not require

- A new renderer. The shadPS4 Vulkan backend already translates GCN programs and builds
  pipelines.
- New resource tracking. The backend resolves guest addresses to host objects.
- Performance work. The capture is within noise.

The gap is semantic association, and the remaining piece of it is the pixel shader.

## Second dependency, after the pixel shader

The vertex declaration. The draw carries a stream index and the stream source carries a buffer
and offset, but nothing maps a stream index to an attribute format. That is a separate RHI
resource and it was not recovered in any of A5 through A9.

Without it, the vertex buffer contents cannot be interpreted, only located.
