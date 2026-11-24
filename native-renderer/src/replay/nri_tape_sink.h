// An NRI-backed consumer of the existing tape::CommandSink contract.
//
// The tape is the authority for draw arguments and command ordering. This sink
// maps each semantic operation onto the semantic RHI, which maps onto NRI. It
// never sees a graphics API and never re-derives draw arguments.
//
// State the tape does not carry is supplied by the caller and reported as a
// substitution. It is never presented as recovered game state.
#pragma once

#include <string>
#include <vector>

#include "common/semantic_rhi.h"
#include "replay/tape.h"

namespace replay {

// Resources the sink needs, resolved by the caller from the package.
struct SinkResources {
    semantic::ResourceId vertexBuffer;
    semantic::ResourceId indexBuffer;
    semantic::ResourceId pipeline;
    semantic::ResourceId colorView;
    semantic::ResourceId depthView;
    semantic::ResourceId descriptorSet;   // optional; 0 when untextured
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t vertexStride = 0;
    // The MVP the diagnostic camera produces. The tape carries no transform.
    const float* mvp = nullptr;
};

class NriTapeSink final : public tape::CommandSink {
public:
    NriTapeSink(semantic::Device& device, const SinkResources& resources);

    void BeginFrame() override;
    void SetViewport(const tape::Viewport& viewport) override;
    void SetRenderTargets(const uint32_t* color_targets, uint32_t color_target_count,
                          uint32_t depth_target) override;
    void BindVertexBuffers(const uint32_t* resources, uint32_t count) override;
    void BindIndexBuffer(uint32_t resource, uint32_t element_size) override;
    void SetGraphicsPipeline(uint32_t pipeline_id) override;
    void BindShaderResources(const uint32_t* resources, uint32_t count) override;
    void DrawIndexed(const tape::DrawPayload& payload, const tape::DerivedDraw& derived) override;
    void EndFrame() override;
    void Opaque(uint64_t sequence, std::string_view note) override;

    // Substitutions this sink applied, for the report.
    const std::vector<std::string>& Substitutions() const { return substitutions_; }
    // The draw arguments actually issued.
    const tape::DerivedDraw& IssuedDraw() const { return issued_; }
    bool IssuedAnyDraw() const { return issuedAny_; }

private:
    void Substitute(const std::string& what);

    semantic::Device& device_;
    SinkResources res_;
    std::vector<std::string> substitutions_;
    tape::DerivedDraw issued_{};
    bool issuedAny_ = false;
    bool inFrame_ = false;
    bool pipelineBound_ = false;
    bool viewportSet_ = false;
    bool scissorSet_ = false;
    bool indexBound_ = false;
    // The captured tape carries no BeginFrame, so the sink opens the pass on the
    // first operation that needs it and records that as a substitution.
    void EnsureFrame();
    void EnsurePipeline();
    void EnsureDrawState();
};

}  // namespace replay
