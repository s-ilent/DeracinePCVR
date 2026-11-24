// A backend-independent semantic RHI boundary.
//
// Three layers are kept strictly separate:
//
//   semantic_rhi.h        backend-independent commands and resource identities
//   backend/nri_device.*  Vulkan/NRI handles, barriers, descriptors, queues
//   (future) game adapter mapping recovered Deracine operations onto this
//
// No Vulkan or NRI type appears in this header. External images are imported
// through an opaque handle so the backend can interpret it without the
// interface depending on Vulkan.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/resource_registry.h"

namespace harness {
class VulkanContext;
}

namespace semantic {

// Resource identities are owned by the registry layer and shared with the
// semantic interface. The alias keeps the semantic header free of backend types.
using ResourceId = harness::ResourceId;

// ---------------------------------------------------------------------------
// Enumerations: only what the tests exercise.
// ---------------------------------------------------------------------------

enum class Format {
    kUnknown,
    kRGBA8Unorm,
    kRGBA8Srgb,
    kR32G32B32A32Float,
    kR32G32B32Float,
    kR32G32Float,
    kRGBA16Float,
    kBC3Unorm,
    kBC3Srgb,
    kD32Float,
    kD24UnormS8,
    kD32FloatS8,
};

enum class ShaderStage { kVertex, kPixel, kCompute };

enum class BufferUsage : uint32_t {
    kNone = 0,
    kVertex = 1u << 0,
    kIndex = 1u << 1,
    kUniform = 1u << 2,
    kStorage = 1u << 3,
    kTransferSrc = 1u << 4,
    kTransferDst = 1u << 5,
};

inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

enum class TextureUsage : uint32_t {
    kNone = 0,
    kColorAttachment = 1u << 0,
    kDepthStencilAttachment = 1u << 1,
    kSampled = 1u << 2,
    kTransferSrc = 1u << 3,
    kTransferDst = 1u << 4,
};

inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

enum class ImageAspect : uint32_t {
    kColor = 1u << 0,
    kDepth = 1u << 1,
    kStencil = 1u << 2,
};

enum class ResourceState {
    kUndefined,
    kColorAttachment,
    kDepthStencilAttachment,
    kShaderRead,
    kTransferSrc,
    kTransferDst,
    kPresent,
};

enum class LoadOp { kLoad, kClear, kDontCare };
enum class StoreOp { kStore, kDontCare };
enum class IndexFormat { kUint16, kUint32 };

// A view used as a shader resource, as opposed to a render target.
enum class ViewKind { kColorAttachment, kDepthStencilAttachment, kSampled };

enum class Filter { kPoint, kLinear };
enum class AddressMode { kClamp, kRepeat };

// ---------------------------------------------------------------------------
// Descriptions
// ---------------------------------------------------------------------------

struct BufferDesc {
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::kNone;
};

struct TextureDesc {
    Format format = Format::kRGBA8Unorm;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t layerNum = 1;
    uint32_t sampleNum = 1;
    TextureUsage usage = TextureUsage::kColorAttachment;
};

// An externally owned image, described without naming a graphics API.
// 'nativeHandle' is interpreted by the backend; the interface never dereferences it.
struct ExternalImageDesc {
    void* nativeHandle = nullptr;   // backend-specific image handle
    Format format = Format::kRGBA8Unorm;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t layerNum = 1;
    uint32_t sampleNum = 1;
    TextureUsage usage = TextureUsage::kColorAttachment;
};

struct TextureViewDesc {
    ResourceId texture;
    Format format = Format::kUnknown;   // kUnknown inherits the texture format
    ImageAspect aspect = ImageAspect::kColor;
    uint32_t baseLayer = 0;
    uint32_t layerNum = 1;
    // A sampled view is a shader resource; an attachment view is a render
    // target. The backend must not force one to be the other.
    ViewKind kind = ViewKind::kColorAttachment;
};

struct SamplerDesc {
    Filter minFilter = Filter::kPoint;
    Filter magFilter = Filter::kPoint;
    AddressMode addressU = AddressMode::kClamp;
    AddressMode addressV = AddressMode::kClamp;
};

// A persistent binding group. The initial implementation supports exactly one
// sampled texture and one sampler, which is what a representative textured draw
// needs. This is deliberately not a general descriptor API.
struct DescriptorSetDesc {
    ResourceId sampledView;   // must be a kSampled texture view
    ResourceId sampler;
};

// A region of a texture to upload into.
struct TextureUploadDesc {
    ResourceId texture;
    const void* data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;    // bytes per row; 0 means tightly packed
    uint32_t layer = 0;
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::kVertex;
    std::vector<uint32_t> bytecode;   // host-native SPIR-V
    std::string entryPoint = "main";
};

struct VertexBinding {
    uint32_t slot = 0;
    uint32_t stride = 0;
};

struct VertexAttribute {
    uint32_t location = 0;
    uint32_t binding = 0;
    Format format = Format::kRGBA8Unorm;
    uint32_t offset = 0;
};

struct ColorAttachment {
    ResourceId view;
    LoadOp loadOp = LoadOp::kClear;
    StoreOp storeOp = StoreOp::kStore;
    float clearColor[4] = {0, 0, 0, 1};
};

struct GraphicsPipelineDesc {
    ResourceId vertexShader;
    ResourceId pixelShader;
    std::vector<VertexBinding> vertexBindings;
    std::vector<VertexAttribute> vertexAttributes;
    std::vector<Format> colorFormats;
    Format depthStencilFormat = Format::kUnknown;
    uint32_t sampleNum = 1;
    bool depthTest = false;
    bool depthWrite = false;
    uint32_t viewMask = 0;   // 0 = sequential; nonzero = multiview
};

struct DepthAttachment {
    ResourceId view;
    LoadOp loadOp = LoadOp::kClear;
    StoreOp storeOp = StoreOp::kStore;
    float clearDepth = 1.0f;
};

struct RenderPassDesc {
    std::vector<ColorAttachment> colorAttachments;
    // Optional depth attachment. Present when the captured workload needs depth.
    bool hasDepth = false;
    DepthAttachment depth;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layerCount = 1;
    uint32_t viewMask = 0;
};

struct Viewport {
    float x = 0, y = 0, width = 0, height = 0, minDepth = 0, maxDepth = 1;
};

struct Scissor {
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

struct DrawIndexedDesc {
    uint32_t indexCount = 0;
    uint32_t instanceCount = 1;
    uint32_t firstIndex = 0;
    int32_t baseVertex = 0;
    uint32_t firstInstance = 0;
};

// A byte-exact parameter payload captured at record time.
//
// The bytes are copied into the command stream, so later mutation of the source
// buffer cannot change what the recorded command observes. This replaces the
// earlier single-float ShaderParameter, which could not express a matrix or a
// constant buffer.
struct ShaderParameterBlock {
    const void* data = nullptr;
    uint32_t sizeBytes = 0;
    ShaderStage visibility = ShaderStage::kVertex;
};

// ---------------------------------------------------------------------------
// The device interface
// ---------------------------------------------------------------------------

class Device {
public:
    virtual ~Device() = default;

    // --- Resources ---
    virtual ResourceId CreateBuffer(const BufferDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateTexture(const TextureDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateTextureView(const TextureViewDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateSampler(const SamplerDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateDescriptorSet(const DescriptorSetDesc& desc,
                                           std::string& error) = 0;
    // Uploads pixels into a texture through the backend. The texture must have
    // been created with kSampled usage.
    virtual bool UploadTexture(const TextureUploadDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateShader(const ShaderDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateGraphicsPipeline(const GraphicsPipelineDesc& desc,
                                              std::string& error) = 0;

    // Imports an externally owned image. The backend must not destroy it.
    virtual ResourceId ImportExternalImage(const ExternalImageDesc& desc,
                                           std::string& error) = 0;

    // Uploads bytes into a buffer. Synchronous; the data is copied.
    virtual bool UploadBuffer(ResourceId buffer, const void* data, uint64_t sizeBytes,
                              uint64_t offset, std::string& error) = 0;

    // Revokes application ownership. Physical destruction may be deferred.
    virtual void Destroy(ResourceId id) = 0;

    // --- Command recording ---
    virtual bool BeginCommands(std::string& error) = 0;
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;
    virtual void SetViewport(const Viewport& vp) = 0;
    virtual void SetScissor(const Scissor& sc) = 0;
    virtual void BindPipeline(ResourceId pipeline) = 0;
    // Binds a descriptor set to the graphics pipeline. Acquires recording
    // ownership of the set and everything it depends on.
    virtual void BindDescriptorSet(ResourceId descriptorSet) = 0;
    virtual void SetVertexBuffer(uint32_t slot, ResourceId buffer, uint64_t offset,
                                 uint32_t stride) = 0;
    virtual void SetIndexBuffer(ResourceId buffer, uint64_t offset, IndexFormat format) = 0;
    virtual void SetShaderParameters(const ShaderParameterBlock& block) = 0;
    virtual void TransitionResource(ResourceId id, ImageAspect aspect, ResourceState from,
                                    ResourceState to) = 0;
    virtual void DrawIndexed(const DrawIndexedDesc& desc) = 0;
    // Ends recording. Returns false if the recording was invalidated by an
    // invalid operation, so a poisoned command buffer cannot be submitted.
    virtual bool EndCommands(std::string& error) = 0;
    // Discards the current recording and releases its references.
    virtual void AbandonRecording() = 0;
    // True when the current recording was invalidated by a rejected operation.
    virtual bool RecordingPoisoned() const = 0;
    // The reason for the most recent rejection, for tests and diagnostics.
    virtual const std::string& LastRejection() const = 0;

    // --- Submission and completion ---
    // Submits the recorded commands. Returns a completion value for the domain.
    virtual bool Submit(uint64_t& outCompletionValue, std::string& error) = 0;
    // Blocks until the given value completes in the graphics domain.
    virtual bool WaitForCompletion(uint64_t value) = 0;
    // Non-blocking query of the current completed value.
    virtual uint64_t CompletedValue() const = 0;

    // --- Verification (test-only, direct Vulkan) ---
    virtual bool ReadbackTexture(ResourceId id, uint32_t layer,
                                 std::vector<unsigned char>& out, std::string& error) = 0;

    // --- Diagnostics ---
    virtual const harness::ResourceRegistry& Registry() const = 0;
    // True when the last recorded command buffer was submitted through NRI.
    virtual bool LastSubmitUsedNri() const = 0;
};

// Creates the NRI-backed device. The Vulkan context must outlive it.
std::unique_ptr<Device> CreateNriDevice(harness::VulkanContext& vk, std::string& error);

}  // namespace semantic
