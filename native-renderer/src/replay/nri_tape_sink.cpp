#include "replay/nri_tape_sink.h"

namespace replay {

NriTapeSink::NriTapeSink(semantic::Device& device, const SinkResources& resources)
    : device_(device), res_(resources) {}

void NriTapeSink::Substitute(const std::string& what) {
    for (const auto& s : substitutions_) {
        if (s == what) return;
    }
    substitutions_.push_back(what);
}

void NriTapeSink::EnsureFrame() {
    if (inFrame_) return;
    BeginFrame();
}

void NriTapeSink::EnsurePipeline() {
    if (pipelineBound_) return;
    SetGraphicsPipeline(0);
}

void NriTapeSink::EnsureDrawState() {
    // The captured tape carries no viewport, scissor or index-buffer operation.
    // Establish them from the caller's target and report each one.
    if (!viewportSet_) {
        Substitute("viewport: diagnostic full-target viewport, the tape carries none");
        tape::Viewport vp;
        vp.min_x = 0.0f;
        vp.min_y = 0.0f;
        vp.min_z = 0.0f;
        vp.max_x = static_cast<float>(res_.width);
        vp.max_y = static_cast<float>(res_.height);
        vp.max_z = 1.0f;
        SetViewport(vp);
        viewportSet_ = true;
    }
    if (!scissorSet_) {
        Substitute("scissor: diagnostic full-target scissor, the tape carries none");
        semantic::Scissor sc;
        sc.width = res_.width;
        sc.height = res_.height;
        device_.SetScissor(sc);
        scissorSet_ = true;
    }
    if (!indexBound_) {
        Substitute("index buffer: package index buffer, the tape carries no bind operation");
        BindIndexBuffer(0, 2);
        indexBound_ = true;
    }
}

void NriTapeSink::BeginFrame() {
    if (inFrame_) return;
    inFrame_ = true;
    // The tape carries no render-target state in this capture, so the caller's
    // target is used and reported.
    Substitute("render target: supplied by the caller, not carried by the tape");
    semantic::RenderPassDesc rp;
    semantic::ColorAttachment ca;
    ca.view = res_.colorView;
    ca.loadOp = semantic::LoadOp::kClear;
    ca.clearColor[0] = 0.05f;
    ca.clearColor[1] = 0.05f;
    ca.clearColor[2] = 0.08f;
    ca.clearColor[3] = 1.0f;
    rp.colorAttachments.push_back(ca);
    if (res_.depthView.IsValid()) {
        rp.hasDepth = true;
        rp.depth.view = res_.depthView;
        rp.depth.loadOp = semantic::LoadOp::kClear;
        rp.depth.clearDepth = 1.0f;
    }
    rp.width = res_.width;
    rp.height = res_.height;
    device_.BeginRenderPass(rp);
}

void NriTapeSink::SetViewport(const tape::Viewport& viewport) {
    semantic::Viewport vp;
    vp.x = viewport.min_x;
    vp.y = viewport.min_y;
    vp.width = viewport.max_x - viewport.min_x;
    vp.height = viewport.max_y - viewport.min_y;
    vp.minDepth = viewport.min_z;
    vp.maxDepth = viewport.max_z;
    device_.SetViewport(vp);
    viewportSet_ = true;
}

void NriTapeSink::SetRenderTargets(const uint32_t* color_targets, uint32_t color_target_count,
                                   uint32_t depth_target) {
    // The tape names its own resource ids; the sink already bound the caller's
    // target in BeginFrame. Record the fact rather than pretending to honour ids
    // that do not map to backend objects.
    Substitute("render target ids from the tape are not mapped to backend objects");
    (void)color_targets;
    (void)color_target_count;
    (void)depth_target;
}

void NriTapeSink::BindVertexBuffers(const uint32_t* resources, uint32_t count) {
    // The tape's vertex-buffer bindings are marked "tentative" by the exporter:
    // the stream-source payload is an interface pointer, not a resource base.
    // The package's validated bindings are used instead, and that is reported.
    Substitute("vertex buffer bindings: package geometry used, tape bindings are tentative");
    (void)resources;
    (void)count;
    device_.SetVertexBuffer(0, res_.vertexBuffer, 0, res_.vertexStride);
}

void NriTapeSink::BindIndexBuffer(uint32_t resource, uint32_t element_size) {
    (void)resource;
    device_.SetIndexBuffer(res_.indexBuffer, 0,
                           element_size == 2 ? semantic::IndexFormat::kUint16
                                             : semantic::IndexFormat::kUint32);
}

void NriTapeSink::SetGraphicsPipeline(uint32_t pipeline_id) {
    // The tape carries no pipeline object; the diagnostic pipeline is used.
    Substitute("graphics pipeline: diagnostic pipeline, the tape carries no pipeline object");
    (void)pipeline_id;
    device_.BindPipeline(res_.pipeline);
    if (res_.descriptorSet.IsValid()) device_.BindDescriptorSet(res_.descriptorSet);
    pipelineBound_ = true;
}

void NriTapeSink::BindShaderResources(const uint32_t* resources, uint32_t count) {
    Substitute("shader resources: diagnostic descriptor set, tape bindings not mapped");
    (void)resources;
    (void)count;
}

void NriTapeSink::DrawIndexed(const tape::DrawPayload& payload, const tape::DerivedDraw& derived) {
    // The captured tape has no BeginFrame or SetGraphicsPipeline operation, so
    // the sink establishes that state on demand and reports it.
    EnsureFrame();
    EnsurePipeline();
    EnsureDrawState();
    // The draw arguments come from the tape's derived values, which the reader
    // recomputed from the authoritative UE4 payload. They are not re-derived here.
    semantic::DrawIndexedDesc draw;
    draw.indexCount = derived.index_count;
    draw.instanceCount = derived.instance_count;
    draw.firstIndex = derived.first_index;
    draw.baseVertex = derived.vertex_offset;
    draw.firstInstance = payload.first_instance;

    if (res_.mvp != nullptr) {
        semantic::ShaderParameterBlock params;
        params.data = res_.mvp;
        params.sizeBytes = 16 * sizeof(float);
        device_.SetShaderParameters(params);
    }
    device_.DrawIndexed(draw);
    issued_ = derived;
    issuedAny_ = true;
}

void NriTapeSink::EndFrame() {
    if (inFrame_) {
        device_.EndRenderPass();
        inFrame_ = false;
    }
}

void NriTapeSink::Opaque(uint64_t sequence, std::string_view note) {
    // Reported, never silently skipped.
    Substitute("opaque tape operation at sequence " + std::to_string(sequence) + ": " +
               std::string(note));
}

}  // namespace replay
