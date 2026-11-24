// A backend-independent semantic RHI boundary.
//
// This interface describes the operations the harness actually exercises, in
// terms of original-engine concepts rather than Vulkan or NRI types. A future
// recovered UE4 RHI layer can target this without knowing which host graphics
// API sits underneath.
//
// Deliberate omissions:
//   * No Vulkan or NRI types appear in this header.
//   * No Déraciné-specific addresses, structures or ABI mappings are invented.
//   * Original resource identity is separate from host object identity.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace semantic {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// An engine-side resource identity. Stable across host object recreation, and
// distinct from any host handle. Generation invalidates stale references.
struct ResourceId {
    uint64_t index = 0;
    uint64_t generation = 0;

    bool operator==(const ResourceId& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const ResourceId& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// Enumerations kept deliberately small: only what the harness exercises.
// ---------------------------------------------------------------------------

enum class Format {
    kUnknown,
    kRGBA8Unorm,
    kRGBA8Srgb,
    kBGRA8Unorm,
    kBGRA8Srgb,
    kR16G16B16A16Float,
    kR32G32B32A32Float,
    kD32Float,
    kD24UnormS8,
    kD32FloatS8,
};

enum class ShaderStage { kVertex, kPixel, kCompute };

enum class ResourceUsage : uint32_t {
    kNone = 0,
    kColorAttachment = 1u << 0,
    kDepthStencilAttachment = 1u << 1,
    kSampled = 1u << 2,
    kStorage = 1u << 3,
    kTransferSrc = 1u << 4,
    kTransferDst = 1u << 5,
};

inline ResourceUsage operator|(ResourceUsage a, ResourceUsage b) {
    return static_cast<ResourceUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
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
    kDepthReadOnly,
    kStencilReadOnly,
    kShaderRead,
    kTransferSrc,
    kTransferDst,
    kPresent,
};

enum class LoadOp { kLoad, kClear, kDontCare };
enum class StoreOp { kStore, kDontCare };

// ---------------------------------------------------------------------------
// Descriptions
// ---------------------------------------------------------------------------

struct TextureDesc {
    Format format = Format::kRGBA8Unorm;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipNum = 1;
    uint32_t layerNum = 1;
    uint32_t sampleNum = 1;
    ResourceUsage usage = ResourceUsage::kColorAttachment;
    bool allowFormatAliasing = false;   // sRGB/linear views over one image
};

struct BufferDesc {
    uint64_t size = 0;
    uint32_t structureStride = 0;
    ResourceUsage usage = ResourceUsage::kNone;
};

struct TextureViewDesc {
    ResourceId texture;
    Format format = Format::kUnknown;   // kUnknown inherits the texture format
    ImageAspect aspect = ImageAspect::kColor;
    uint32_t baseMip = 0;
    uint32_t mipNum = 1;
    uint32_t baseLayer = 0;
    uint32_t layerNum = 1;
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::kVertex;
    std::vector<uint32_t> bytecode;   // host-native SPIR-V for this backend
    std::string entryPoint = "main";
};

struct VertexInputBinding {
    uint32_t slot = 0;
    uint32_t stride = 0;
};

struct VertexAttribute {
    uint32_t location = 0;
    uint32_t binding = 0;
    Format format = Format::kRGBA8Unorm;
    uint32_t offset = 0;
};

struct ColorAttachmentDesc {
    ResourceId view;
    LoadOp loadOp = LoadOp::kClear;
    StoreOp storeOp = StoreOp::kStore;
    float clearColor[4] = {0, 0, 0, 1};
};

struct DepthStencilAttachmentDesc {
    ResourceId view;
    LoadOp depthLoadOp = LoadOp::kClear;
    StoreOp depthStoreOp = StoreOp::kStore;
    LoadOp stencilLoadOp = LoadOp::kClear;
    StoreOp stencilStoreOp = StoreOp::kStore;
    float clearDepth = 1.0f;
    uint32_t clearStencil = 0;
};

struct GraphicsPipelineDesc {
    ResourceId vertexShader;
    ResourceId pixelShader;
    std::vector<VertexInputBinding> vertexBindings;
    std::vector<VertexAttribute> vertexAttributes;
    std::vector<Format> colorFormats;
    Format depthStencilFormat = Format::kUnknown;
    uint32_t sampleNum = 1;
    bool depthTest = false;
    bool depthWrite = false;
    bool stencilTest = false;
    uint32_t viewMask = 0;   // 0 = sequential; nonzero = multiview
};

struct RenderPassDesc {
    std::vector<ColorAttachmentDesc> colorAttachments;
    DepthStencilAttachmentDesc depthStencil;
    bool hasDepthStencil = false;
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

// A scalar shader parameter captured at record time. The value is copied, so
// later mutation of the source cannot change what the command observes.
struct ShaderParameter {
    uint32_t slot = 0;
    float value = 0.0f;
};

// ---------------------------------------------------------------------------
// The device interface
// ---------------------------------------------------------------------------

class Device {
public:
    virtual ~Device() = default;

    // Resources
    virtual ResourceId CreateTexture(const TextureDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateBuffer(const BufferDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateTextureView(const TextureViewDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateShader(const ShaderDesc& desc, std::string& error) = 0;
    virtual ResourceId CreateGraphicsPipeline(const GraphicsPipelineDesc& desc,
                                              std::string& error) = 0;
    virtual void Destroy(ResourceId id) = 0;

    // Command recording
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;
    virtual void SetViewport(const Viewport& vp) = 0;
    virtual void SetScissor(const Scissor& sc) = 0;
    virtual void BindPipeline(ResourceId pipeline) = 0;
    virtual void SetShaderParameters(const ShaderParameter* params, uint32_t count) = 0;
    virtual void TransitionResource(ResourceId id, ImageAspect aspect, ResourceState from,
                                    ResourceState to) = 0;
    virtual void ClearColor(ResourceId view, const float rgba[4]) = 0;
    virtual void ClearDepthStencil(ResourceId view, float depth, uint32_t stencil) = 0;
    virtual void CopyResource(ResourceId dst, ResourceId src) = 0;
    virtual void ResolveResource(ResourceId dst, ResourceId src) = 0;
    virtual void DrawIndexed(const DrawIndexedDesc& desc) = 0;
    virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;

    // Submission and completion
    virtual uint64_t Submit() = 0;              // returns a completion value
    virtual bool WaitForCompletion(uint64_t value) = 0;
    virtual uint64_t CompletedValue() const = 0;

    // Readback for verification
    virtual bool ReadbackTexture(ResourceId id, uint32_t layer,
                                 std::vector<unsigned char>& out, std::string& error) = 0;
};

// Creates the backend. The harness supplies a Vulkan/NRI implementation; a
// different backend can be substituted without touching callers.
std::unique_ptr<Device> CreateVulkanNriDevice(std::string& error);

}  // namespace semantic
