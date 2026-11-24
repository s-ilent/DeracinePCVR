// deracine_nri_replay: renders a captured Deracine draw through the native NRI
// renderer.
//
// Input is an existing SC-09H or SC-10S package directory. The package is the
// authority for geometry, indices, texture bytes and sampler state. Everything
// this program substitutes is printed at startup and recorded in the report.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backend/nri_device.h"
#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/shader_compile.h"
#include "common/vk_bootstrap.h"
#include "replay/package.h"

namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 640;

// A 4x4 column-major matrix, matching the reference replay's convention.
struct Mat4 {
    float m[16]{};
};

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + r] * b.m[c * 4 + k];
            out.m[c * 4 + r] = sum;
        }
    }
    return out;
}

Mat4 Perspective(float fovY, float aspect, float nearZ, float farZ) {
    Mat4 out{};
    const float f = 1.0f / std::tan(fovY * 0.5f);
    out.m[0] = f / aspect;
    out.m[5] = f;
    out.m[10] = farZ / (nearZ - farZ);
    out.m[11] = -1.0f;
    out.m[14] = (nearZ * farZ) / (nearZ - farZ);
    return out;
}

Mat4 LookAt(const float eye[3], const float target[3], const float up[3]) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl > 1e-6f) { f[0] /= fl; f[1] /= fl; f[2] /= fl; }
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                  f[0] * up[1] - f[1] * up[0]};
    const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl > 1e-6f) { s[0] /= sl; s[1] /= sl; s[2] /= sl; }
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                        s[0] * f[1] - s[1] * f[0]};
    Mat4 out{};
    out.m[0] = s[0]; out.m[4] = s[1]; out.m[8] = s[2];
    out.m[1] = u[0]; out.m[5] = u[1]; out.m[9] = u[2];
    out.m[2] = -f[0]; out.m[6] = -f[1]; out.m[10] = -f[2];
    out.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    out.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    out.m[14] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    out.m[15] = 1.0f;   // Identity() in the reference sets this
    return out;
}

// Half-float to float, matching the reference replay's conversion.
float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 0x1u;
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t mantissa = h & 0x3FFu;
    float value = 0.0f;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? NAN : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

// Matches the reference replay's scene.vert: colour by object-space position so
// the three axes are distinguishable.
const char* kVs = R"(
#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec3 v_color;
layout(location = 1) out vec2 v_uv;
layout(push_constant) uniform Push { mat4 mvp; } pc;
void main() {
    v_color = clamp(in_pos * 0.5 + 0.5, 0.0, 1.0);
    v_uv = in_uv;
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
}
)";

const char* kFs = R"(
#version 450
layout(location = 0) in vec3 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform texture2D u_tex;
layout(set = 0, binding = 1) uniform sampler u_samp;
void main() {
    vec4 texel = texture(sampler2D(u_tex, u_samp), v_uv);
    out_color = vec4(texel.rgb, 1.0);
}
)";

// Diagnostic-only shader for the untextured geometry path.
// Matches the reference replay's scene.frag.
const char* kFsFlat = R"(
#version 450
layout(location = 0) in vec3 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = vec4(v_color, 1.0);
}
)";

semantic::Format ToSemanticFormat(uint32_t vkFormat) {
    switch (vkFormat) {
        case 106: return semantic::Format::kR32G32B32Float;
        case 37: return semantic::Format::kRGBA8Unorm;
        case 43: return semantic::Format::kRGBA8Srgb;
        case 97: return semantic::Format::kRGBA16Float;
        case 138: return semantic::Format::kBC3Srgb;
        case 137: return semantic::Format::kBC3Unorm;
        default: return semantic::Format::kUnknown;
    }
}

// Writes a PPM in the same row order as the reference replay, so the two images
// are directly comparable. The reference's readback rows are bottom-up relative
// to ours; flipping here makes a byte diff meaningful.
void WritePPM(const std::string& path, const std::vector<unsigned char>& rgba, uint32_t w,
              uint32_t h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t srcRow = h - 1 - y;
        for (uint32_t x = 0; x < w; ++x) {
            std::fwrite(&rgba[(static_cast<size_t>(srcRow) * w + x) * 4], 1, 3, f);
        }
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    harness::TestLog log("deracine_nri_replay");

    if (argc < 2) {
        std::printf("usage: deracine_nri_replay <package-dir> [--out <dir>]\n");
        return 2;
    }
    const std::string packageDir = argv[1];
    std::string outDir = packageDir;

    replay::Package pkg;
    std::string error;
    if (!replay::LoadPackage(packageDir, pkg, error)) {
        log.Fail("package load: " + error);
        return log.Finish();
    }
    log.Pass("package loaded: scene=" + pkg.scene +
             " provenance=" + std::to_string(pkg.provenance_id));

    std::printf("  [info] resources=%zu vertex_bindings=%zu textures=%zu samplers=%zu\n",
                pkg.resources.size(), pkg.vertex_bindings.size(), pkg.textures.size(),
                pkg.samplers.size());
    std::printf("  [info] index_count=%u instance_count=%u triangles=%u\n",
                pkg.index_binding.index_count, pkg.index_binding.instance_count,
                pkg.geometry.triangle_count);

    // ---- Vulkan + NRI ----
    harness::BootstrapConfig cfg;
    cfg.enableValidation = true;
    cfg.enableSyncValidation = true;
    harness::VulkanContext vk;
    if (!vk.Initialize(cfg, error)) {
        log.Fail("Vulkan init: " + error);
        return log.Finish();
    }
    log.Pass("physical GPU: " + vk.manifest().deviceName);

    auto device = semantic::CreateNriDevice(vk, error);
    if (!device) {
        log.Fail("semantic device: " + error);
        return log.Finish();
    }
    log.Pass("semantic NRI device created");

    // ---- Output target ----
    harness::ImageDesc imgDesc;
    imgDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgDesc.width = kWidth;
    imgDesc.height = kHeight;
    imgDesc.layerNum = 1;
    imgDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    harness::Image target;
    if (!harness::CreateImage(vk, imgDesc, target, error)) {
        log.Fail("target image: " + error);
        return log.Finish();
    }
    VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());
    const float bg[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    harness::ClearColor(vk, pool, target, bg, 0);

    semantic::ExternalImageDesc extDesc;
    extDesc.nativeHandle = reinterpret_cast<void*>(target.image);
    extDesc.format = semantic::Format::kRGBA8Unorm;
    extDesc.width = kWidth;
    extDesc.height = kHeight;
    extDesc.layerNum = 1;
    extDesc.usage = semantic::TextureUsage::kColorAttachment |
                    semantic::TextureUsage::kTransferSrc;
    const auto targetId = device->ImportExternalImage(extDesc, error);
    semantic::TextureViewDesc tvd;
    tvd.texture = targetId;
    tvd.aspect = semantic::ImageAspect::kColor;
    tvd.kind = semantic::ViewKind::kColorAttachment;
    const auto targetView = device->CreateTextureView(tvd, error);
    if (!targetId.IsValid() || !targetView.IsValid()) {
        log.Fail("target import: " + error);
        return log.Finish();
    }
    // Depth target, matching the reference replay's D32 attachment.
    harness::ImageDesc depthDesc;
    depthDesc.format = VK_FORMAT_D32_SFLOAT;
    depthDesc.width = kWidth;
    depthDesc.height = kHeight;
    depthDesc.layerNum = 1;
    depthDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    harness::Image depthImage;
    if (!harness::CreateImage(vk, depthDesc, depthImage, error)) {
        log.Fail("depth image: " + error);
        return log.Finish();
    }
    semantic::ExternalImageDesc depthExt;
    depthExt.nativeHandle = reinterpret_cast<void*>(depthImage.image);
    depthExt.format = semantic::Format::kD32Float;
    depthExt.width = kWidth;
    depthExt.height = kHeight;
    depthExt.layerNum = 1;
    depthExt.usage = semantic::TextureUsage::kDepthStencilAttachment;
    const auto depthId = device->ImportExternalImage(depthExt, error);
    semantic::TextureViewDesc dvd;
    dvd.texture = depthId;
    dvd.aspect = semantic::ImageAspect::kDepth;
    dvd.kind = semantic::ViewKind::kDepthStencilAttachment;
    const auto depthView = device->CreateTextureView(dvd, error);
    if (!depthId.IsValid() || !depthView.IsValid()) {
        log.Fail("depth view: " + error);
        return log.Finish();
    }
    log.Pass("render target and depth target created");

    // ---- Build the diagnostic vertex stream ----
    // Position comes from the attributed position binding; UV from the
    // attributed texture-coordinate binding. Both are captured data.
    const replay::VertexBinding* posBinding = nullptr;
    const replay::VertexBinding* uvBinding = nullptr;
    for (const auto& vb : pkg.vertex_bindings) {
        if (vb.binding == pkg.geometry.position_binding) posBinding = &vb;
        if (vb.binding == pkg.texture_coordinate.binding) uvBinding = &vb;
    }
    const bool textured = !pkg.textures.empty() && uvBinding != nullptr &&
                          pkg.texture_coordinate.looks_like_uv;
    if (posBinding == nullptr) {
        log.Fail("position binding missing from package");
        return log.Finish();
    }
    const replay::Resource* posRes = pkg.GetResource(posBinding->resource_ref);
    if (posRes == nullptr) {
        log.Fail("position resource missing");
        return log.Finish();
    }
    const replay::Resource* uvRes =
        uvBinding != nullptr ? pkg.GetResource(uvBinding->resource_ref) : nullptr;

    const uint32_t vertexCount =
        static_cast<uint32_t>(posRes->bytes.size() / posBinding->stride);
    std::printf("  [info] position binding=%u stride=%u format=%s vertices=%u\n",
                posBinding->binding, posBinding->stride,
                replay::FormatName(posBinding->vk_format), vertexCount);
    if (uvBinding != nullptr) {
        std::printf("  [info] uv binding=%u stride=%u format=%s components=%u,%u\n",
                    uvBinding->binding, uvBinding->stride,
                    replay::FormatName(uvBinding->vk_format),
                    pkg.texture_coordinate.component_u, pkg.texture_coordinate.component_v);
    }

    // Normalise into a diagnostic interleaved stream: position + UV.
    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        lo[i] = pkg.geometry.bounds_min[i];
        hi[i] = pkg.geometry.bounds_max[i];
    }
    float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float extent = 0.0f;
    for (int c = 0; c < 3; ++c) extent = std::max(extent, hi[c] - lo[c]);
    const float scale = extent > 1e-6f ? 2.0f / extent : 1.0f;
    std::printf("  [info] diagnostic transform: centre=(%.3f,%.3f,%.3f) scale=%.6f\n",
                centre[0], centre[1], centre[2], scale);

    struct Vertex {
        float x, y, z;
        float u, v;
    };
    std::vector<Vertex> vertices(vertexCount);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        Vertex out{};
        // Accumulate all three components. Resetting the vector inside the loop
        // would keep only the last axis.
        float pos[3] = {0.0f, 0.0f, 0.0f};
        for (int c = 0; c < 3; ++c) {
            float value = 0.0f;
            if (static_cast<uint32_t>(c) < posBinding->element_count) {
                std::memcpy(&value,
                            posRes->bytes.data() + v * posBinding->stride + c * 4,
                            sizeof(value));
            }
            pos[c] = (value - centre[c]) * scale;
        }
        out.x = pos[0];
        out.y = pos[1];
        out.z = pos[2];
        if (uvRes != nullptr && uvBinding != nullptr) {
            const uint32_t comps[2] = {pkg.texture_coordinate.component_u,
                                       pkg.texture_coordinate.component_v};
            float uv[2] = {0.0f, 0.0f};
            for (int c = 0; c < 2; ++c) {
                if (uvBinding->vk_format == 97) {   // R16G16B16A16_SFLOAT
                    uint16_t raw = 0;
                    std::memcpy(&raw, uvRes->bytes.data() + v * uvBinding->stride + comps[c] * 2,
                                sizeof(raw));
                    uv[c] = HalfToFloat(raw);
                } else {
                    std::memcpy(&uv[c],
                                uvRes->bytes.data() + v * uvBinding->stride + comps[c] * 4,
                                sizeof(float));
                }
            }
            out.u = uv[0];
            out.v = uv[1];
        }
        vertices[v] = out;
    }

    // ---- Index buffer ----
    const replay::Resource* idxRes = pkg.GetResource(pkg.index_binding.resource_ref);
    if (idxRes == nullptr) {
        log.Fail("index resource missing");
        return log.Finish();
    }
    log.Pass("captured geometry and indices resolved");

    // ---- Upload geometry ----
    semantic::BufferDesc vbDesc;
    vbDesc.size = vertices.size() * sizeof(Vertex);
    vbDesc.usage = semantic::BufferUsage::kVertex;
    const auto vb = device->CreateBuffer(vbDesc, error);
    if (!vb.IsValid() || !device->UploadBuffer(vb, vertices.data(), vbDesc.size, 0, error)) {
        log.Fail("vertex buffer: " + error);
        return log.Finish();
    }
    semantic::BufferDesc ibDesc;
    ibDesc.size = idxRes->bytes.size();
    ibDesc.usage = semantic::BufferUsage::kIndex;
    const auto ib = device->CreateBuffer(ibDesc, error);
    if (!ib.IsValid() || !device->UploadBuffer(ib, idxRes->bytes.data(), ibDesc.size, 0, error)) {
        log.Fail("index buffer: " + error);
        return log.Finish();
    }
    log.Pass("captured geometry uploaded to NRI buffers");

    // ---- Texture ----
    semantic::ResourceId sampledView;
    semantic::ResourceId sampler;
    semantic::ResourceId descriptorSet;
    if (textured) {
        const auto& tex = pkg.textures[0];
        const auto& sub = tex.subresources[0];
        std::printf("  [info] texture: %ux%u format=%s blocks=%ux%u bytes=%llu\n", tex.width,
                    tex.height, replay::FormatName(tex.image_format), tex.block_columns,
                    tex.block_rows, static_cast<unsigned long long>(sub.byte_length));

        semantic::TextureDesc td;
        td.format = ToSemanticFormat(tex.image_format);
        td.width = tex.width;
        td.height = tex.height;
        td.usage = semantic::TextureUsage::kSampled;
        const auto texId = device->CreateTexture(td, error);
        if (!texId.IsValid()) {
            log.Fail("texture create: " + error);
            return log.Finish();
        }
        semantic::TextureUploadDesc up;
        up.texture = texId;
        up.data = pkg.texture_blob.data() + sub.blob_offset;
        up.width = tex.width;
        up.height = tex.height;
        up.rowPitch = sub.row_pitch;
        if (!device->UploadTexture(up, error)) {
            log.Fail("texture upload: " + error);
            return log.Finish();
        }
        log.Pass("captured texture uploaded through NRI");

        semantic::TextureViewDesc svd;
        svd.texture = texId;
        svd.aspect = semantic::ImageAspect::kColor;
        svd.kind = semantic::ViewKind::kSampled;
        sampledView = device->CreateTextureView(svd, error);

        const replay::Sampler* s = pkg.FindSampler(tex.sampler_id);
        semantic::SamplerDesc sd;
        if (s != nullptr) {
            sd.minFilter = s->min_filter ? semantic::Filter::kLinear : semantic::Filter::kPoint;
            sd.magFilter = s->mag_filter ? semantic::Filter::kLinear : semantic::Filter::kPoint;
            // Captured address mode 2 is clamp; 0 is repeat.
            sd.addressU = (s->address_u == 0) ? semantic::AddressMode::kRepeat
                                              : semantic::AddressMode::kClamp;
            sd.addressV = (s->address_v == 0) ? semantic::AddressMode::kRepeat
                                              : semantic::AddressMode::kClamp;
            std::printf("  [info] sampler %u: min=%u mag=%u addr=%u,%u verified=%d\n", s->id,
                        s->min_filter, s->mag_filter, s->address_u, s->address_v,
                        tex.sampler_verified ? 1 : 0);
        }
        sampler = device->CreateSampler(sd, error);
        semantic::DescriptorSetDesc ds;
        ds.sampledView = sampledView;
        ds.sampler = sampler;
        descriptorSet = device->CreateDescriptorSet(ds, error);
        if (!sampledView.IsValid() || !sampler.IsValid() || !descriptorSet.IsValid()) {
            log.Fail("descriptor setup: " + error);
            return log.Finish();
        }
        log.Pass("sampled view, sampler and descriptor set created");
    } else {
        log.Info("package has no usable texture; rendering untextured diagnostic");
    }

    // ---- Pipeline ----
    std::vector<uint32_t> vsSpv, fsSpv;
    if (!harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error) ||
        !harness::CompileGlsl(textured ? kFs : kFsFlat, VK_SHADER_STAGE_FRAGMENT_BIT, "main",
                              fsSpv, error)) {
        log.Fail("shader compile: " + error);
        return log.Finish();
    }
    semantic::ShaderDesc vsd;
    vsd.stage = semantic::ShaderStage::kVertex;
    vsd.bytecode = vsSpv;
    const auto vs = device->CreateShader(vsd, error);
    semantic::ShaderDesc fsd;
    fsd.stage = semantic::ShaderStage::kPixel;
    fsd.bytecode = fsSpv;
    const auto fs = device->CreateShader(fsd, error);
    semantic::GraphicsPipelineDesc pd;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.vertexBindings.push_back({0, sizeof(Vertex)});
    pd.vertexAttributes.push_back({0, 0, semantic::Format::kR32G32B32Float, 0});
    pd.vertexAttributes.push_back({1, 0, semantic::Format::kR32G32Float, 12});
    pd.colorFormats.push_back(semantic::Format::kRGBA8Unorm);
    pd.depthStencilFormat = semantic::Format::kD32Float;
    pd.depthTest = true;
    pd.depthWrite = true;
    const auto pipe = device->CreateGraphicsPipeline(pd, error);
    if (!pipe.IsValid()) {
        log.Fail("pipeline: " + error);
        return log.Finish();
    }
    log.Pass("diagnostic pipeline created");

    // ---- Record and submit ----
    const float eye[3] = {0.0f, 0.0f, 3.2f};
    const float look[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 mvp = Multiply(Perspective(0.9f, 1.0f, 0.1f, 100.0f), LookAt(eye, look, up));

    if (!device->BeginCommands(error)) {
        log.Fail("BeginCommands: " + error);
        return log.Finish();
    }
    device->TransitionResource(targetId, semantic::ImageAspect::kColor,
                               semantic::ResourceState::kUndefined,
                               semantic::ResourceState::kColorAttachment);
    device->TransitionResource(depthId, semantic::ImageAspect::kDepth,
                               semantic::ResourceState::kUndefined,
                               semantic::ResourceState::kDepthStencilAttachment);
    semantic::RenderPassDesc rp;
    semantic::ColorAttachment ca;
    ca.view = targetView;
    ca.loadOp = semantic::LoadOp::kLoad;
    rp.colorAttachments.push_back(ca);
    rp.hasDepth = true;
    rp.depth.view = depthView;
    rp.depth.loadOp = semantic::LoadOp::kClear;
    rp.depth.storeOp = semantic::StoreOp::kStore;
    rp.depth.clearDepth = 1.0f;
    rp.width = kWidth;
    rp.height = kHeight;
    device->BeginRenderPass(rp);
    device->BindPipeline(pipe);
    if (textured) device->BindDescriptorSet(descriptorSet);
    semantic::Viewport vp;
    vp.width = static_cast<float>(kWidth);
    vp.height = static_cast<float>(kHeight);
    device->SetViewport(vp);
    semantic::Scissor sc;
    sc.width = kWidth;
    sc.height = kHeight;
    device->SetScissor(sc);
    device->SetVertexBuffer(0, vb, 0, sizeof(Vertex));
    device->SetIndexBuffer(ib, 0, pkg.index_binding.index_size == 2
                                      ? semantic::IndexFormat::kUint16
                                      : semantic::IndexFormat::kUint32);
    semantic::ShaderParameterBlock params;
    params.data = mvp.m;
    params.sizeBytes = sizeof(mvp.m);
    device->SetShaderParameters(params);
    semantic::DrawIndexedDesc draw;
    draw.indexCount = pkg.index_binding.index_count;
    draw.instanceCount = pkg.index_binding.instance_count;
    draw.firstIndex = pkg.index_binding.first_index;
    draw.baseVertex = pkg.index_binding.base_vertex;
    draw.firstInstance = pkg.index_binding.first_instance;
    device->DrawIndexed(draw);
    device->EndRenderPass();
    if (!device->EndCommands(error)) {
        log.Fail("EndCommands: " + error);
        return log.Finish();
    }
    uint64_t completion = 0;
    if (!device->Submit(completion, error)) {
        log.Fail("Submit: " + error);
        return log.Finish();
    }
    device->WaitForCompletion(completion);
    log.Pass("captured draw submitted through NRI and completed");

    // ---- Readback ----
    std::vector<unsigned char> pixels;
    if (!harness::ReadbackColor(vk, pool, target, 0, pixels, error)) {
        log.Fail("readback: " + error);
        return log.Finish();
    }
    WritePPM(outDir + "/nri_replay.ppm", pixels, kWidth, kHeight);

    // Coverage: count pixels that differ from the background.
    uint32_t covered = 0;
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const unsigned char* p = &pixels[i * 4];
        if (p[0] > 20 || p[1] > 20 || p[2] > 28) ++covered;
    }
    const double coverage = 100.0 * covered / (kWidth * kHeight);
    std::printf("  [info] coverage: %u px (%.2f%%)\n", covered, coverage);
    if (covered > 0) {
        log.Pass("captured mesh produced visible coverage");
    } else {
        log.Fail("captured mesh produced no coverage");
    }

    device->Destroy(descriptorSet);
    device->Destroy(sampler);
    device->Destroy(sampledView);
    device->Destroy(vb);
    device->Destroy(ib);
    device->Destroy(vs);
    device->Destroy(fs);
    device->Destroy(pipe);
    device->Destroy(depthView);
    device->Destroy(depthId);
    device->Destroy(targetView);
    device->Destroy(targetId);
    device.reset();
    harness::DestroyImage(vk, depthImage);
    harness::DestroyImage(vk, target);
    vkDestroyCommandPool(vk.device(), pool, nullptr);
    vk.Shutdown();

    const uint32_t unexpected = harness::UnexpectedErrorCount();
    if (unexpected == 0) {
        log.Pass("zero unexpected validation errors");
    } else {
        log.Fail(std::to_string(unexpected) + " unexpected validation errors");
        for (const auto& msg : harness::GlobalValidationCounters().messages) log.Info(msg);
    }
    return log.Finish();
}
