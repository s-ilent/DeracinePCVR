// deracine_nri_tape: replays a semantic command tape through the NRI backend.
//
// The tape is the authority for draw arguments and command ordering. The package
// supplies the geometry and texture bytes the tape references but does not carry.
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
#include "replay/nri_tape_sink.h"
#include "replay/package.h"
#include "replay/tape.h"

namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 640;

struct Mat4 { float m[16]{}; };

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + r] * b.m[c * 4 + k];
            out.m[c * 4 + r] = s;
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
    out.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    out.m[15] = 1.0f;
    return out;
}

const char* kVs = R"(
#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 0) out vec3 v_color;
layout(push_constant) uniform Push { mat4 mvp; } pc;
void main() {
    v_color = clamp(in_pos * 0.5 + 0.5, 0.0, 1.0);
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
}
)";

const char* kFs = R"(
#version 450
layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(v_color, 1.0); }
)";

void WritePPM(const std::string& path, const std::vector<unsigned char>& rgba, uint32_t w,
              uint32_t h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t srcRow = h - 1 - y;
        for (uint32_t x = 0; x < w; ++x)
            std::fwrite(&rgba[(static_cast<size_t>(srcRow) * w + x) * 4], 1, 3, f);
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    harness::TestLog log("deracine_nri_tape");

    if (argc < 2) {
        std::printf("usage: deracine_nri_tape <package-dir-with-tape>\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::string error;

    // ---- Read and validate the tape ----
    tape::Tape t;
    if (!tape::ReadTape(dir, t, error)) {
        log.Fail("tape read: " + error);
        return log.Finish();
    }
    const tape::Problem problem = tape::ValidateTape(t);
    if (problem != tape::Problem::None) {
        log.Fail(std::string("tape invalid: ") + std::string(tape::ProblemName(problem)));
        return log.Finish();
    }
    log.Pass("tape validated: scene=" + t.scene +
             " provenance=" + std::to_string(t.provenance_id) +
             " operations=" + std::to_string(t.operations.size()));

    // ---- Read the package for geometry ----
    replay::Package pkg;
    if (!replay::LoadPackage(dir, pkg, error)) {
        log.Fail("package load: " + error);
        return log.Finish();
    }
    // The tape and the package must describe the same draw.
    if (pkg.provenance_id != t.provenance_id) {
        log.Fail("tape provenance " + std::to_string(t.provenance_id) +
                 " does not match package provenance " + std::to_string(pkg.provenance_id));
        return log.Finish();
    }
    log.Pass("tape and package agree on provenance " + std::to_string(t.provenance_id));

    // ---- Vulkan + NRI ----
    harness::BootstrapConfig cfg;
    cfg.enableValidation = true;
    cfg.enableSyncValidation = true;
    harness::VulkanContext vk;
    if (!vk.Initialize(cfg, error)) {
        log.Fail("Vulkan init: " + error);
        return log.Finish();
    }
    auto device = semantic::CreateNriDevice(vk, error);
    if (!device) {
        log.Fail("semantic device: " + error);
        return log.Finish();
    }
    log.Pass("semantic NRI device created on " + vk.manifest().deviceName);

    // ---- Targets ----
    harness::ImageDesc cd;
    cd.format = VK_FORMAT_R8G8B8A8_UNORM;
    cd.width = kWidth;
    cd.height = kHeight;
    cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    harness::Image color;
    harness::ImageDesc dd;
    dd.format = VK_FORMAT_D32_SFLOAT;
    dd.width = kWidth;
    dd.height = kHeight;
    dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    harness::Image depth;
    if (!harness::CreateImage(vk, cd, color, error) ||
        !harness::CreateImage(vk, dd, depth, error)) {
        log.Fail("targets: " + error);
        return log.Finish();
    }
    VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());

    semantic::ExternalImageDesc ce;
    ce.nativeHandle = reinterpret_cast<void*>(color.image);
    ce.format = semantic::Format::kRGBA8Unorm;
    ce.width = kWidth;
    ce.height = kHeight;
    ce.usage = semantic::TextureUsage::kColorAttachment | semantic::TextureUsage::kTransferSrc;
    const auto colorId = device->ImportExternalImage(ce, error);
    semantic::TextureViewDesc cv;
    cv.texture = colorId;
    cv.aspect = semantic::ImageAspect::kColor;
    cv.kind = semantic::ViewKind::kColorAttachment;
    const auto colorView = device->CreateTextureView(cv, error);

    semantic::ExternalImageDesc de;
    de.nativeHandle = reinterpret_cast<void*>(depth.image);
    de.format = semantic::Format::kD32Float;
    de.width = kWidth;
    de.height = kHeight;
    de.usage = semantic::TextureUsage::kDepthStencilAttachment;
    const auto depthId = device->ImportExternalImage(de, error);
    semantic::TextureViewDesc dv;
    dv.texture = depthId;
    dv.aspect = semantic::ImageAspect::kDepth;
    dv.kind = semantic::ViewKind::kDepthStencilAttachment;
    const auto depthView = device->CreateTextureView(dv, error);
    if (!colorView.IsValid() || !depthView.IsValid()) {
        log.Fail("target views: " + error);
        return log.Finish();
    }

    // ---- Geometry from the package ----
    const replay::VertexBinding* posBinding = nullptr;
    for (const auto& vb : pkg.vertex_bindings) {
        if (vb.binding == pkg.geometry.position_binding) posBinding = &vb;
    }
    const replay::Resource* posRes =
        posBinding != nullptr ? pkg.GetResource(posBinding->resource_ref) : nullptr;
    const replay::Resource* idxRes = pkg.GetResource(pkg.index_binding.resource_ref);
    if (posRes == nullptr || idxRes == nullptr) {
        log.Fail("package geometry missing");
        return log.Finish();
    }
    const uint32_t vertexCount =
        static_cast<uint32_t>(posRes->bytes.size() / posBinding->stride);

    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        lo[i] = pkg.geometry.bounds_min[i];
        hi[i] = pkg.geometry.bounds_max[i];
    }
    float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float extent = 0.0f;
    for (int c = 0; c < 3; ++c) extent = std::max(extent, hi[c] - lo[c]);
    const float scale = extent > 1e-6f ? 2.0f / extent : 1.0f;

    struct Vertex { float x, y, z; };
    std::vector<Vertex> verts(vertexCount);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        float pos[3] = {0, 0, 0};
        for (int c = 0; c < 3; ++c) {
            float value = 0.0f;
            if (static_cast<uint32_t>(c) < posBinding->element_count) {
                std::memcpy(&value, posRes->bytes.data() + v * posBinding->stride + c * 4,
                            sizeof(value));
            }
            pos[c] = (value - centre[c]) * scale;
        }
        verts[v] = Vertex{pos[0], pos[1], pos[2]};
    }

    semantic::BufferDesc vbd;
    vbd.size = verts.size() * sizeof(Vertex);
    vbd.usage = semantic::BufferUsage::kVertex;
    const auto vb = device->CreateBuffer(vbd, error);
    if (!vb.IsValid() || !device->UploadBuffer(vb, verts.data(), vbd.size, 0, error)) {
        log.Fail("vertex buffer: " + error);
        return log.Finish();
    }
    semantic::BufferDesc ibd;
    ibd.size = idxRes->bytes.size();
    ibd.usage = semantic::BufferUsage::kIndex;
    const auto ib = device->CreateBuffer(ibd, error);
    if (!ib.IsValid() || !device->UploadBuffer(ib, idxRes->bytes.data(), ibd.size, 0, error)) {
        log.Fail("index buffer: " + error);
        return log.Finish();
    }

    // ---- Pipeline ----
    std::vector<uint32_t> vsSpv, fsSpv;
    if (!harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error) ||
        !harness::CompileGlsl(kFs, VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error)) {
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
    pd.colorFormats.push_back(semantic::Format::kRGBA8Unorm);
    pd.depthStencilFormat = semantic::Format::kD32Float;
    pd.depthTest = true;
    pd.depthWrite = true;
    const auto pipe = device->CreateGraphicsPipeline(pd, error);
    if (!pipe.IsValid()) {
        log.Fail("pipeline: " + error);
        return log.Finish();
    }

    // ---- Execute the tape into the NRI sink ----
    const float eye[3] = {0.0f, 0.0f, 3.2f};
    const float look[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 mvp = Multiply(Perspective(0.9f, 1.0f, 0.1f, 100.0f), LookAt(eye, look, up));

    replay::SinkResources sr;
    sr.vertexBuffer = vb;
    sr.indexBuffer = ib;
    sr.pipeline = pipe;
    sr.colorView = colorView;
    sr.depthView = depthView;
    sr.width = kWidth;
    sr.height = kHeight;
    sr.vertexStride = sizeof(Vertex);
    sr.mvp = mvp.m;

    if (!device->BeginCommands(error)) {
        log.Fail("BeginCommands: " + error);
        return log.Finish();
    }
    device->TransitionResource(colorId, semantic::ImageAspect::kColor,
                               semantic::ResourceState::kUndefined,
                               semantic::ResourceState::kColorAttachment);
    device->TransitionResource(depthId, semantic::ImageAspect::kDepth,
                               semantic::ResourceState::kUndefined,
                               semantic::ResourceState::kDepthStencilAttachment);

    replay::NriTapeSink sink(*device, sr);
    tape::ExecutionReport report;

    // The captured tape carries no viewport operation. Supply the diagnostic
    // viewport explicitly through the contract's substitution mechanism, so the
    // report names it rather than the sink inventing it silently.
    tape::Substitutions subs;
    subs.has_viewport = true;
    subs.viewport.min_x = 0.0f;
    subs.viewport.min_y = 0.0f;
    subs.viewport.min_z = 0.0f;
    subs.viewport.max_x = static_cast<float>(kWidth);
    subs.viewport.max_y = static_cast<float>(kHeight);
    subs.viewport.max_z = 1.0f;

    if (!tape::Execute(t, sink, report, error, subs)) {
        log.Fail("tape execute: " + error);
        return log.Finish();
    }
    for (const auto& s : report.substituted) {
        std::printf("  [info] interpreter substitution: %s\n", s.c_str());
    }
    // The tape has no EndFrame either; close the pass the sink opened.
    sink.EndFrame();
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

    log.Pass("tape executed: " + std::to_string(report.operations_executed) + " operations, " +
             std::to_string(report.draws) + " draws, " +
             std::to_string(report.opaque_operations) + " opaque");
    std::printf("  [info] issued draw: index_count=%u first_index=%u vertex_offset=%d "
                "instance_count=%u\n",
                sink.IssuedDraw().index_count, sink.IssuedDraw().first_index,
                sink.IssuedDraw().vertex_offset, sink.IssuedDraw().instance_count);
    if (sink.IssuedAnyDraw()) log.Pass("a draw was issued from the tape's derived arguments");
    else log.Fail("no draw was issued");

    // ---- Readback ----
    std::vector<unsigned char> pixels;
    if (!harness::ReadbackColor(vk, pool, color, 0, pixels, error)) {
        log.Fail("readback: " + error);
        return log.Finish();
    }
    WritePPM(dir + "/nri_tape.ppm", pixels, kWidth, kHeight);
    uint32_t covered = 0;
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const unsigned char* p = &pixels[i * 4];
        if (p[0] > 20 || p[1] > 20 || p[2] > 28) ++covered;
    }
    std::printf("  [info] coverage: %u px (%.2f%%)\n", covered,
                100.0 * covered / (kWidth * kHeight));
    if (covered > 0) log.Pass("tape-driven draw produced visible coverage");
    else log.Fail("tape-driven draw produced no coverage");

    // ---- Report substitutions honestly ----
    std::printf("  [info] substitutions applied by the sink:\n");
    for (const auto& s : sink.Substitutions()) std::printf("    - %s\n", s.c_str());
    if (!sink.Substitutions().empty()) {
        log.Pass("every substitution is named, none presented as recovered state");
    }

    device->Destroy(vb);
    device->Destroy(ib);
    device->Destroy(vs);
    device->Destroy(fs);
    device->Destroy(pipe);
    device->Destroy(colorView);
    device->Destroy(depthView);
    device->Destroy(colorId);
    device->Destroy(depthId);
    device.reset();
    harness::DestroyImage(vk, color);
    harness::DestroyImage(vk, depth);
    vkDestroyCommandPool(vk.device(), pool, nullptr);
    vk.Shutdown();

    const uint32_t unexpected = harness::UnexpectedErrorCount();
    if (unexpected == 0) log.Pass("zero unexpected validation errors");
    else log.Fail(std::to_string(unexpected) + " unexpected validation errors");
    return log.Finish();
}
