// NRI implementation of the semantic RHI.
//
// Responsibility split:
//   NRI    - command recording, pipeline binding, barriers, draws, submission
//   Vulkan - device creation, externally owned images, test-only readback
//
// The backend never renders through direct Vulkan. Readback is the only Vulkan
// operation performed on the render path, and it is test-only.
//
// Resource identity is owned by ResourceRegistry. Every lookup validates index,
// generation, type and lifecycle state, so a stale id can never reach a freed
// NRI object. Backend payloads are owned by the registry and deleted by its
// destruction callback.
#pragma once

#include <vulkan/vulkan.h>

#include <NRI.h>
#include <Extensions/NRIWrapperVK.h>

#include <memory>
#include <string>
#include <vector>

#include "common/resource_registry.h"
#include "common/semantic_rhi.h"
#include "common/vk_bootstrap.h"

namespace harness {

class NriContext;

namespace detail {

struct BufferPayload {
    nri::Buffer* buffer = nullptr;
    uint64_t size = 0;
};

struct TexturePayload {
    nri::Texture* texture = nullptr;
    bool external = false;   // imported; the VkImage is not ours
    VkImage vkImage = VK_NULL_HANDLE;
    semantic::Format format = semantic::Format::kUnknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layerNum = 1;
    uint32_t sampleNum = 1;
};

struct ViewPayload {
    nri::Descriptor* descriptor = nullptr;
    ResourceId texture;      // the parent this view depends on
    semantic::ImageAspect aspect = semantic::ImageAspect::kColor;
    uint32_t baseLayer = 0;
    uint32_t layerNum = 1;
};

struct ShaderPayload {
    std::vector<uint32_t> spirv;
    semantic::ShaderStage stage = semantic::ShaderStage::kVertex;
};

struct PipelinePayload {
    nri::Pipeline* pipeline = nullptr;
    nri::PipelineLayout* layout = nullptr;
    uint32_t viewMask = 0;
};

}  // namespace detail

// One reusable recording slot. A context is only reset once its command buffer
// is known to have completed.
struct CommandContext {
    enum class State { kAvailable, kRecording, kExecutable, kSubmitted, kCompleted };

    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* buffer = nullptr;
    State state = State::kAvailable;
    uint64_t submitValue = 0;   // the fence value this context signalled
    uint64_t recordingToken = 0;
};

class NriDevice final : public semantic::Device {
public:
    NriDevice();
    ~NriDevice() override;

    bool Initialize(VulkanContext& vk, std::string& error);

    // --- Resources ---
    ResourceId CreateBuffer(const semantic::BufferDesc& desc, std::string& error) override;
    ResourceId CreateTexture(const semantic::TextureDesc& desc, std::string& error) override;
    ResourceId CreateTextureView(const semantic::TextureViewDesc& desc,
                                 std::string& error) override;
    ResourceId CreateShader(const semantic::ShaderDesc& desc, std::string& error) override;
    ResourceId CreateGraphicsPipeline(const semantic::GraphicsPipelineDesc& desc,
                                      std::string& error) override;
    ResourceId ImportExternalImage(const semantic::ExternalImageDesc& desc,
                                   std::string& error) override;
    bool UploadBuffer(ResourceId buffer, const void* data, uint64_t sizeBytes, uint64_t offset,
                      std::string& error) override;
    void Destroy(ResourceId id) override;

    // --- Command recording ---
    bool BeginCommands(std::string& error) override;
    void BeginRenderPass(const semantic::RenderPassDesc& desc) override;
    void EndRenderPass() override;
    void SetViewport(const semantic::Viewport& vp) override;
    void SetScissor(const semantic::Scissor& sc) override;
    void BindPipeline(ResourceId pipeline) override;
    void SetVertexBuffer(uint32_t slot, ResourceId buffer, uint64_t offset,
                         uint32_t stride) override;
    void SetIndexBuffer(ResourceId buffer, uint64_t offset, semantic::IndexFormat format) override;
    void SetShaderParameters(const semantic::ShaderParameterBlock& block) override;
    void TransitionResource(ResourceId id, semantic::ImageAspect aspect,
                            semantic::ResourceState from,
                            semantic::ResourceState to) override;
    void DrawIndexed(const semantic::DrawIndexedDesc& desc) override;
    bool EndCommands(std::string& error) override;

    // --- Submission ---
    bool Submit(uint64_t& outCompletionValue, std::string& error) override;
    bool WaitForCompletion(uint64_t value) override;
    uint64_t CompletedValue() const override;

    // --- Verification ---
    bool ReadbackTexture(ResourceId id, uint32_t layer, std::vector<unsigned char>& out,
                         std::string& error) override;

    // --- Diagnostics ---
    const ResourceRegistry& Registry() const override { return registry_; }
    bool LastSubmitUsedNri() const override { return lastSubmitUsedNri_; }

    // True when the current recording was poisoned by an invalid operation.
    bool RecordingPoisoned() const { return poisoned_; }
    // Number of submissions accepted but not yet observed complete. This is
    // backend bookkeeping, not a claim about simultaneous GPU execution.
    int OutstandingSubmissionCount() const;
    // Number of command contexts currently available for recording.
    int AvailableContextCount() const;
    // The last rejection reason, for tests and diagnostics.
    const std::string& LastRejection() const { return lastRejection_; }

    // Abandons the current recording, releasing its references.
    void AbandonRecording();

    // Releases everything. Drains outstanding work first.
    void Shutdown();

private:
    // Validates and retains a resource for the active recording. On failure the
    // recording is poisoned so it cannot be submitted as if it succeeded.
    bool Acquire(ResourceId id, ResourceType type, const char* what);
    void Reject(const std::string& reason);

    // Returns a context whose command buffer is known to be complete, or null
    // when the pool is exhausted.
    CommandContext* AcquireContext();
    // Retires contexts whose submit value has completed.
    void ReapCompletedContexts();
    // Blocks until at least one context is available.
    bool WaitForContext(std::string& error);

    VulkanContext* vk_ = nullptr;
    std::unique_ptr<NriContext> nri_;

    nri::Queue* queue_ = nullptr;
    nri::Fence* fence_ = nullptr;
    nri::PipelineLayout* activeLayout_ = nullptr;
    nri::Pipeline* activePipeline_ = nullptr;

    ResourceRegistry registry_;

    std::vector<CommandContext> contexts_;
    CommandContext* active_ = nullptr;

    uint64_t nextSubmitValue_ = 1;
    uint64_t completedValue_ = 0;
    bool lastSubmitUsedNri_ = false;

    bool recording_ = false;
    bool poisoned_ = false;
    std::string lastRejection_;

    // Parameter bytes are copied here so the recorded command owns them.
    std::vector<unsigned char> paramScratch_;
};

}  // namespace harness
