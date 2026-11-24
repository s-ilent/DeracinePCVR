// NRI implementation of the semantic RHI.
//
// Responsibility split:
//   NRI   - command recording, pipeline binding, barriers, draws, submission
//   Vulkan - device creation, externally owned images, test-only readback
//
// The backend never renders through direct Vulkan. Readback is the only Vulkan
// operation performed on the render path, and it is test-only.
#pragma once

#include <vulkan/vulkan.h>

#include <NRI.h>
#include <Extensions/NRIWrapperVK.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/semantic_rhi.h"
#include "common/vk_bootstrap.h"

namespace harness {

class NriContext;

namespace detail {

using ResourceId = harness::ResourceId;

// Backend payloads stored behind a ResourceId.
struct BufferPayload {
    nri::Buffer* buffer = nullptr;
    uint64_t size = 0;
};

struct TexturePayload {
    nri::Texture* texture = nullptr;
    bool external = false;   // imported; must not be destroyed by us
    VkImage vkImage = VK_NULL_HANDLE;
    semantic::Format format = semantic::Format::kUnknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layerNum = 1;
    uint32_t sampleNum = 1;
};

struct ViewPayload {
    nri::Descriptor* descriptor = nullptr;
    ResourceId texture;
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

    // Releases everything. Safe to call with outstanding work.
    void Shutdown();

private:
    // Records a GPU reference for every resource touched by the current
    // command buffer, so it survives until submission completes.
    void RetainForCurrentRecording(ResourceId id);

    VulkanContext* vk_ = nullptr;
    std::unique_ptr<NriContext> nri_;

    nri::Queue* queue_ = nullptr;
    nri::CommandAllocator* cmdAllocator_ = nullptr;
    nri::CommandBuffer* cmdBuffer_ = nullptr;
    nri::Fence* fence_ = nullptr;
    nri::PipelineLayout* activeLayout_ = nullptr;
    nri::Pipeline* activePipeline_ = nullptr;

    ResourceRegistry registry_;

    // Payload lookup by resource index.
    std::unordered_map<uint64_t, detail::BufferPayload> buffers_;
    std::unordered_map<uint64_t, detail::TexturePayload> textures_;
    std::unordered_map<uint64_t, detail::ViewPayload> views_;
    std::unordered_map<uint64_t, detail::ShaderPayload> shaders_;
    std::unordered_map<uint64_t, detail::PipelinePayload> pipelines_;

    // Resources referenced by the command buffer currently being recorded.
    std::vector<ResourceId> recordingRefs_;
    bool recording_ = false;

    uint64_t nextSubmitValue_ = 1;
    uint64_t completedValue_ = 0;
    bool lastSubmitUsedNri_ = false;

    // Parameter bytes are copied here so the recorded command owns them.
    std::vector<unsigned char> paramScratch_;
};

}  // namespace harness
