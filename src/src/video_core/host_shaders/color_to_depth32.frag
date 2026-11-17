// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Writes a single-sample R32 float picture into a depth attachment, one pixel per pixel.
//
// The guest keeps one allocation in an R32F surface and then uses the same words as a depth
// buffer: D32 depth values are IEEE floats, so the words are the value. Vulkan has no view or
// alias that lets the emulator share the memory between the two formats, and an image copy from
// a colour format into a depth one costs the driver a detile and decompress of the whole depth
// surface. Reading the words as a texture and writing them through the depth unit costs one pass.

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

layout (binding = 0, set = 0) uniform texture2D source_color;

void main() {
    gl_FragDepth = texelFetch(source_color, ivec2(gl_FragCoord.xy), 0).r;
}
