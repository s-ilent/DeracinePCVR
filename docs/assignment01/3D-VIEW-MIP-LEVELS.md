# 2D/2DArray views over a mipmapped 3D image ask for more than one level

Status: **recorded, not fixed.** Latent — no title has been shown to hit it.

## What the rule is

`VUID-VkImageViewCreateInfo-image-04970`: if the image was created with `VK_IMAGE_TYPE_3D` and
`viewType` is `VK_IMAGE_VIEW_TYPE_2D` or `VK_IMAGE_VIEW_TYPE_2D_ARRAY`, then
`subresourceRange.levelCount` **must be 1**.

A 2D view of a 3D image may slice the volume (that is what
`VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT` / `VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT` are for,
and `Image::Image` sets both for a volume), but it may not span mip levels: the slices of one
level are the layers, and there is no defined meaning for a level-covering 2D view of a volume.

Reference: <https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateInfo.html>

## Why this tree can produce one

- `IsViewTypeCompatible` (`src/src/video_core/texture_cache/image_view.h:35`) answers **true** for a
  `Color2D`/`Color2DArray` view over a `Color3D` backing, so the cache is free to hand a volume to
  a 2D/2DArray request (the expansion path is only taken when sizes/resources do not line up).
- `ImageViewInfo::ImageViewInfo(const AmdGpu::Image&, const Shader::ImageResource&)`
  (`src/src/video_core/texture_cache/image_view.cpp:51`) then sets
  `range.extent.levels = image.NumViewLevels(desc.is_array)`, which is derived from the guest T#.
  Nothing in the constructor knows that the *backing* is a volume, so the "must be 1" rule is never
  applied. `NumViewLevels` already clamps against `NumLevels()` (the #5453 cap), but that cap is a
  per-axis mip-chain bound, not this rule.
- The only guard on this path logs when the view type is *incompatible*, and it is compatible, so
  the invalid view is created silently.

## Effect

The image view is invalid usage; the sampled result for that view is undefined rather than a
wrong-but-defined value. In practice a driver may accept it, so the symptom would be a mip or
slice artefact on a volume only, and only with `>1` level visible through a 2D/2DArray T#.

## What it is not

- Not the 32³ colour-grading LUT of CUSA11919 (`0x2008f80000`): its 3D image is single-level
  (`levels=1` in the alias trace), and a single-level view has `levelCount == 1`. The LUT's
  2D↔3D conversions are unaffected.
- Not a wrap/clamp defect: the W address mode question is separate and is covered by
  `docs/assignment01/LUT-SAMPLER-STATE.md`.

## Fix direction (nothing applied)

In `ImageViewInfo`, when the requested view type is `Color2D`/`Color2DArray` and the backing image
is `Color3D`, clamp `range.extent.levels` to 1 (and note the clamp), or refuse the view so the
cache takes its re-create/expand path, as it already does in the opposite direction. Either way a
unit test can assert the rule on `ImageViewInfo` directly, with no GPU needed.
