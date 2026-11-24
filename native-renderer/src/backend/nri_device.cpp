#include "backend/nri_device.h"

#include <cstring>

#include "common/image_io.h"
#include "common/nri_adoption.h"

namespace harness {

namespace {

nri::Format ToNriFormat(semantic::Format f) {
    switch (f) {
        case semantic::Format::kRGBA8Unorm: return nri::Format::RGBA8_UNORM;
        case semantic::Format::kRGBA8Srgb: return nri::Format::RGBA8_SRGB;
        case semantic::Format::kR32G32B32A32Float: return nri::Format::RGBA32_SFLOAT;
        case semantic::Format::kR32G32Float: return nri::Format::RG32_SFLOAT;
        case semantic::Format::kD32Float: return nri::Format::D32_SFLOAT;
        case semantic::Format::kD24UnormS8: return nri::Format::D24_UNORM_S8_UINT;
        case semantic::Format::kD32FloatS8: return nri::Format::D32_SFLOAT_S8_UINT;
        default: return nri::Format::UNKNOWN;
    }
}

nri::BufferUsageBits ToNriBufferUsage(semantic::BufferUsage u) {
    uint32_t bits = 0;
    const auto raw = static_cast<uint32_t>(u);
    if (raw & static_cast<uint32_t>(semantic::BufferUsage::kVertex)) bits |= static_cast<uint32_t>(nri::BufferUsageBits::VERTEX);
    if (raw & static_cast<uint32_t>(semantic::BufferUsage::kIndex)) bits |= static_cast<uint32_t>(nri::BufferUsageBits::INDEX);
    if (raw & static_cast<uint32_t>(semantic::BufferUsage::kUniform)) bits |= static_cast<uint32_t>(nri::BufferUsageBits::CONSTANT);
    if (raw & static_cast<uint32_t>(semantic::BufferUsage::kStorage)) bits |= static_cast<uint32_t>(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
    return static_cast<nri::BufferUsageBits>(bits);
}

nri::TextureUsageBits ToNriTextureUsage(semantic::TextureUsage u) {
    uint32_t bits = 0;
    const auto raw = static_cast<uint32_t>(u);
    if (raw & static_cast<uint32_t>(semantic::TextureUsage::kColorAttachment)) bits |= static_cast<uint32_t>(nri::TextureUsageBits::COLOR_ATTACHMENT);
    if (raw & static_cast<uint32_t>(semantic::TextureUsage::kDepthStencilAttachment)) bits |= static_cast<uint32_t>(nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT);
    if (raw & static_cast<uint32_t>(semantic::TextureUsage::kSampled)) bits |= static_cast<uint32_t>(nri::TextureUsageBits::SHADER_RESOURCE);
    return static_cast<nri::TextureUsageBits>(bits);
}

nri::PlaneBits ToNriPlanes(semantic::ImageAspect a) {
    uint32_t bits = 0;
    const auto raw = static_cast<uint32_t>(a);
    if (raw & static_cast<uint32_t>(semantic::ImageAspect::kColor)) bits |= static_cast<uint32_t>(nri::PlaneBits::COLOR);
    if (raw & static_cast<uint32_t>(semantic::ImageAspect::kDepth)) bits |= static_cast<uint32_t>(nri::PlaneBits::DEPTH);
    if (raw & static_cast<uint32_t>(semantic::ImageAspect::kStencil)) bits |= static_cast<uint32_t>(nri::PlaneBits::STENCIL);
    return static_cast<nri::PlaneBits>(bits);
}

nri::Layout ToNriLayout(semantic::ResourceState s) {
    switch (s) {
        case semantic::ResourceState::kColorAttachment: return nri::Layout::COLOR_ATTACHMENT;
        case semantic::ResourceState::kDepthStencilAttachment: return nri::Layout::DEPTH_STENCIL_ATTACHMENT;
        case semantic::ResourceState::kShaderRead: return nri::Layout::SHADER_RESOURCE;
        case semantic::ResourceState::kTransferSrc: return nri::Layout::COPY_SOURCE;
        case semantic::ResourceState::kTransferDst: return nri::Layout::COPY_DESTINATION;
        case semantic::ResourceState::kPresent: return nri::Layout::PRESENT;
        default: return nri::Layout::UNDEFINED;
    }
}

nri::AccessBits ToNriAccess(semantic::ResourceState s) {
    switch (s) {
        case semantic::ResourceState::kColorAttachment:
            return static_cast<nri::AccessBits>(
                static_cast<uint32_t>(nri::AccessBits::COLOR_ATTACHMENT_WRITE));
        case semantic::ResourceState::kDepthStencilAttachment:
            return static_cast<nri::AccessBits>(
                static_cast<uint32_t>(nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE));
        case semantic::ResourceState::kShaderRead:
            return static_cast<nri::AccessBits>(
                static_cast<uint32_t>(nri::AccessBits::SHADER_RESOURCE));
        case semantic::ResourceState::kTransferSrc:
            return static_cast<nri::AccessBits>(static_cast<uint32_t>(nri::AccessBits::COPY_SOURCE));
        case semantic::ResourceState::kTransferDst:
            return static_cast<nri::AccessBits>(static_cast<uint32_t>(nri::AccessBits::COPY_DESTINATION));
        default:
            return static_cast<nri::AccessBits>(static_cast<uint32_t>(nri::AccessBits::NONE));
    }
}

nri::StageBits ToNriStages(semantic::ResourceState s) {
    switch (s) {
        case semantic::ResourceState::kColorAttachment:
            return static_cast<nri::StageBits>(static_cast<uint32_t>(nri::StageBits::COLOR_ATTACHMENT));
        case semantic::ResourceState::kDepthStencilAttachment:
            return static_cast<nri::StageBits>(static_cast<uint32_t>(nri::StageBits::DEPTH_STENCIL_ATTACHMENT));
        case semantic::ResourceState::kShaderRead:
            return static_cast<nri::StageBits>(
                static_cast<uint32_t>(nri::StageBits::VERTEX_SHADER) |
                static_cast<uint32_t>(nri::StageBits::FRAGMENT_SHADER));
        case semantic::ResourceState::kTransferSrc:
        case semantic::ResourceState::kTransferDst:
            return static_cast<nri::StageBits>(static_cast<uint32_t>(nri::StageBits::COPY));
        default:
            return static_cast<nri::StageBits>(static_cast<uint32_t>(nri::StageBits::ALL));
    }
}

}  // namespace

NriDevice::NriDevice() = default;

NriDevice::~NriDevice() { Shutdown(); }

bool NriDevice::Initialize(VulkanContext& vk, std::string& error) {
    vk_ = &vk;
    nri_ = std::make_unique<NriContext>();
    if (!nri_->Initialize(vk, true, error)) return false;

    if (nri_->core().GetQueue(*nri_->device(), nri::QueueType::GRAPHICS, 0, queue_) !=
            nri::Result::SUCCESS ||
        queue_ == nullptr) {
        error = "GetQueue failed";
        return false;
    }

    if (nri_->core().CreateCommandAllocator(*queue_, cmdAllocator_) != nri::Result::SUCCESS ||
        cmdAllocator_ == nullptr) {
        error = "CreateCommandAllocator failed";
        return false;
    }
    if (nri_->core().CreateCommandBuffer(*cmdAllocator_, cmdBuffer_) != nri::Result::SUCCESS ||
        cmdBuffer_ == nullptr) {
        error = "CreateCommandBuffer failed";
        return false;
    }

    nri::FenceDesc fd{};
    fd.initialValue = 0;
    if (nri_->core().CreateFence(*nri_->device(), fd, fence_) != nri::Result::SUCCESS ||
        fence_ == nullptr) {
        error = "CreateFence failed";
        return false;
    }
    return true;
}

void NriDevice::Shutdown() {
    if (nri_ && nri_->device() != nullptr) {
        nri_->core().DeviceWaitIdle(nri_->device());
    }
    // Drain the registry: the device is idle, so nothing can still reference.
    registry_.DestroyAll();
    buffers_.clear();
    textures_.clear();
    views_.clear();
    shaders_.clear();
    pipelines_.clear();

    if (nri_ && nri_->device() != nullptr) {
        if (fence_) nri_->core().DestroyFence(fence_);
        if (cmdBuffer_) nri_->core().DestroyCommandBuffer(cmdBuffer_);
        if (cmdAllocator_) nri_->core().DestroyCommandAllocator(cmdAllocator_);
        fence_ = nullptr;
        cmdBuffer_ = nullptr;
        cmdAllocator_ = nullptr;
        nri_->Shutdown();
    }
    nri_.reset();
}

void NriDevice::RetainForCurrentRecording(ResourceId id) {
    if (!recording_) return;
    for (const auto& r : recordingRefs_) {
        if (r == id) return;
    }
    recordingRefs_.push_back(id);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

ResourceId NriDevice::CreateBuffer(const semantic::BufferDesc& desc, std::string& error) {
    nri::BufferDesc bd{};
    bd.size = desc.size;
    bd.usage = ToNriBufferUsage(desc.usage);

    // Buffers the caller uploads into must live in host-visible memory, because
    // MapBuffer only works on HOST_UPLOAD allocations. Vertex and index buffers
    // are small in this harness, so host-upload is the correct choice.
    nri::Buffer* buf = nullptr;
    if (nri_->core().CreateCommittedBuffer(*nri_->device(), nri::MemoryLocation::HOST_UPLOAD,
                                           0.0f, bd, buf) != nri::Result::SUCCESS ||
        buf == nullptr) {
        error = "CreateCommittedBuffer failed";
        return {};
    }

    auto payload = std::make_unique<detail::BufferPayload>();
    payload->buffer = buf;
    payload->size = desc.size;
    detail::BufferPayload* raw = payload.get();

    ResourceId id = registry_.Create(raw, [this](void* p) {
        auto* bp = static_cast<detail::BufferPayload*>(p);
        if (bp->buffer) nri_->core().DestroyBuffer(bp->buffer);
        buffers_.erase(0);   // payload ownership handled below
        delete bp;
    });
    buffers_[id.index] = *raw;
    payload.release();
    return id;
}

ResourceId NriDevice::CreateTexture(const semantic::TextureDesc& desc, std::string& error) {
    nri::TextureDesc td{};
    td.type = nri::TextureType::TEXTURE_2D;
    td.usage = ToNriTextureUsage(desc.usage);
    td.format = ToNriFormat(desc.format);
    td.width = static_cast<nri::Dim_t>(desc.width);
    td.height = static_cast<nri::Dim_t>(desc.height);
    td.depth = 1;
    td.mipNum = 1;
    td.layerNum = static_cast<nri::Dim_t>(desc.layerNum);
    td.sampleNum = static_cast<nri::Sample_t>(desc.sampleNum);

    nri::Texture* tex = nullptr;
    if (nri_->core().CreateCommittedTexture(*nri_->device(), nri::MemoryLocation::DEVICE, 0.0f, td,
                                            tex) != nri::Result::SUCCESS ||
        tex == nullptr) {
        error = "CreateCommittedTexture failed";
        return {};
    }

    auto* payload = new detail::TexturePayload();
    payload->texture = tex;
    payload->external = false;
    payload->format = desc.format;
    payload->width = desc.width;
    payload->height = desc.height;
    payload->layerNum = desc.layerNum;
    payload->sampleNum = desc.sampleNum;

    ResourceId id = registry_.Create(payload, [this](void* p) {
        auto* tp = static_cast<detail::TexturePayload*>(p);
        if (tp->texture && !tp->external) nri_->core().DestroyTexture(tp->texture);
        delete tp;
    });
    textures_[id.index] = *payload;
    return id;
}

ResourceId NriDevice::ImportExternalImage(const semantic::ExternalImageDesc& desc,
                                          std::string& error) {
    if (desc.nativeHandle == nullptr) {
        error = "ImportExternalImage: null handle";
        return {};
    }
    const VkImage image = static_cast<VkImage>(desc.nativeHandle);

    // The wrapper needs the real Vulkan format and usage flags. The semantic
    // layer describes them abstractly, so map them back here.
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    switch (desc.format) {
        case semantic::Format::kRGBA8Unorm: vkFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
        case semantic::Format::kRGBA8Srgb: vkFormat = VK_FORMAT_R8G8B8A8_SRGB; break;
        case semantic::Format::kR32G32B32A32Float: vkFormat = VK_FORMAT_R32G32B32A32_SFLOAT; break;
        case semantic::Format::kR32G32Float: vkFormat = VK_FORMAT_R32G32_SFLOAT; break;
        case semantic::Format::kD32Float: vkFormat = VK_FORMAT_D32_SFLOAT; break;
        case semantic::Format::kD24UnormS8: vkFormat = VK_FORMAT_D24_UNORM_S8_UINT; break;
        case semantic::Format::kD32FloatS8: vkFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; break;
        default: break;
    }
    if (vkFormat == VK_FORMAT_UNDEFINED) {
        error = "ImportExternalImage: unknown format";
        return {};
    }

    VkImageUsageFlags vkUsage = 0;
    const auto uraw = static_cast<uint32_t>(desc.usage);
    if (uraw & static_cast<uint32_t>(semantic::TextureUsage::kColorAttachment))
        vkUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (uraw & static_cast<uint32_t>(semantic::TextureUsage::kDepthStencilAttachment))
        vkUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (uraw & static_cast<uint32_t>(semantic::TextureUsage::kSampled))
        vkUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (uraw & static_cast<uint32_t>(semantic::TextureUsage::kTransferSrc))
        vkUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (uraw & static_cast<uint32_t>(semantic::TextureUsage::kTransferDst))
        vkUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    nri::Texture* tex = nullptr;
    if (!nri_->WrapTexture(image, vkFormat, VK_IMAGE_TYPE_2D, vkUsage, desc.width,
                           desc.height, 1, 1, desc.layerNum, desc.sampleNum, tex, error)) {
        return {};
    }

    auto* payload = new detail::TexturePayload();
    payload->texture = tex;
    payload->external = true;   // never destroyed by us
    payload->vkImage = image;
    payload->format = desc.format;
    payload->width = desc.width;
    payload->height = desc.height;
    payload->layerNum = desc.layerNum;
    payload->sampleNum = desc.sampleNum;

    ResourceId id = registry_.Create(payload, [this](void* p) {
        auto* tp = static_cast<detail::TexturePayload*>(p);
        // Destroy the NRI wrapper only. The VkImage belongs to the caller.
        if (tp->texture) nri_->core().DestroyTexture(tp->texture);
        delete tp;
    });
    textures_[id.index] = *payload;
    return id;
}

ResourceId NriDevice::CreateTextureView(const semantic::TextureViewDesc& desc,
                                        std::string& error) {
    auto tit = textures_.find(desc.texture.index);
    if (tit == textures_.end()) {
        error = "CreateTextureView: unknown texture";
        return {};
    }
    const auto& tp = tit->second;

    nri::TextureViewDesc vd{};
    vd.texture = tp.texture;
    // A view used as a render target must be declared as an attachment view,
    // not a shader-resource view. The usage check in NRI enforces this.
    if (desc.aspect == semantic::ImageAspect::kColor) {
        vd.type = nri::TextureView::COLOR_ATTACHMENT;
    } else {
        vd.type = nri::TextureView::DEPTH_STENCIL_ATTACHMENT;
    }
    vd.format = (desc.format == semantic::Format::kUnknown) ? nri::Format::UNKNOWN
                                                            : ToNriFormat(desc.format);
    vd.mipOffset = 0;
    vd.mipNum = 1;
    vd.layerOffset = static_cast<nri::Dim_t>(desc.baseLayer);
    vd.layerNum = static_cast<nri::Dim_t>(desc.layerNum);
    vd.planes = ToNriPlanes(desc.aspect);

    nri::Descriptor* view = nullptr;
    if (nri_->core().CreateTextureView(vd, view) != nri::Result::SUCCESS || view == nullptr) {
        error = "CreateTextureView failed";
        return {};
    }

    auto* payload = new detail::ViewPayload();
    payload->descriptor = view;
    payload->texture = desc.texture;
    payload->aspect = desc.aspect;
    payload->baseLayer = desc.baseLayer;
    payload->layerNum = desc.layerNum;

    ResourceId id = registry_.Create(payload, [this](void* p) {
        auto* vp = static_cast<detail::ViewPayload*>(p);
        if (vp->descriptor) nri_->core().DestroyDescriptor(vp->descriptor);
        delete vp;
    });
    views_[id.index] = *payload;
    return id;
}

ResourceId NriDevice::CreateShader(const semantic::ShaderDesc& desc, std::string& error) {
    if (desc.bytecode.empty()) {
        error = "CreateShader: empty bytecode";
        return {};
    }
    auto* payload = new detail::ShaderPayload();
    payload->spirv = desc.bytecode;
    payload->stage = desc.stage;

    ResourceId id = registry_.Create(payload, [](void* p) {
        delete static_cast<detail::ShaderPayload*>(p);
    });
    shaders_[id.index] = *payload;
    return id;
}

ResourceId NriDevice::CreateGraphicsPipeline(const semantic::GraphicsPipelineDesc& desc,
                                             std::string& error) {
    auto vsIt = shaders_.find(desc.vertexShader.index);
    auto psIt = shaders_.find(desc.pixelShader.index);
    if (vsIt == shaders_.end() || psIt == shaders_.end()) {
        error = "CreateGraphicsPipeline: unknown shader";
        return {};
    }

    // A single root-constant block carries the per-draw parameters.
    nri::RootConstantDesc rc{};
    rc.registerIndex = 0;
    rc.size = 64;   // 4x vec4, enough for a matrix or per-eye color
    rc.shaderStages = static_cast<nri::StageBits>(
        static_cast<uint32_t>(nri::StageBits::VERTEX_SHADER) |
        static_cast<uint32_t>(nri::StageBits::FRAGMENT_SHADER));

    nri::PipelineLayoutDesc pld{};
    pld.rootRegisterSpace = 0;
    pld.rootConstants = &rc;
    pld.rootConstantNum = 1;
    pld.shaderStages = rc.shaderStages;

    nri::PipelineLayout* layout = nullptr;
    if (nri_->core().CreatePipelineLayout(*nri_->device(), pld, layout) != nri::Result::SUCCESS ||
        layout == nullptr) {
        error = "CreatePipelineLayout failed";
        return {};
    }

    nri::ShaderDesc shaders[2]{};
    shaders[0].stage = nri::StageBits::VERTEX_SHADER;
    shaders[0].bytecode = vsIt->second.spirv.data();
    shaders[0].size = vsIt->second.spirv.size() * sizeof(uint32_t);
    shaders[0].entryPointName = "main";
    shaders[1].stage = nri::StageBits::FRAGMENT_SHADER;
    shaders[1].bytecode = psIt->second.spirv.data();
    shaders[1].size = psIt->second.spirv.size() * sizeof(uint32_t);
    shaders[1].entryPointName = "main";

    std::vector<nri::VertexAttributeDesc> attrs;
    for (const auto& a : desc.vertexAttributes) {
        nri::VertexAttributeDesc va{};
        va.vk.location = a.location;
        va.d3d.semanticName = "ATTR";
        va.d3d.semanticIndex = a.location;
        va.offset = a.offset;
        va.format = ToNriFormat(a.format);
        va.streamIndex = static_cast<uint16_t>(a.binding);
        attrs.push_back(va);
    }
    std::vector<nri::VertexStreamDesc> streams;
    for (const auto& b : desc.vertexBindings) {
        nri::VertexStreamDesc vs{};
        vs.bindingSlot = static_cast<uint16_t>(b.slot);
        vs.stepRate = nri::VertexStreamStepRate::PER_VERTEX;
        vs.stride = static_cast<uint16_t>(b.stride);
        streams.push_back(vs);
    }
    nri::VertexInputDesc vid{};
    vid.attributes = attrs.empty() ? nullptr : attrs.data();
    vid.attributeNum = static_cast<uint8_t>(attrs.size());
    vid.streams = streams.empty() ? nullptr : streams.data();
    vid.streamNum = static_cast<uint8_t>(streams.size());

    nri::InputAssemblyDesc iad{};
    iad.topology = nri::Topology::TRIANGLE_LIST;

    nri::RasterizationDesc rd{};
    rd.fillMode = nri::FillMode::SOLID;
    rd.cullMode = nri::CullMode::NONE;
    rd.frontCounterClockwise = true;

    nri::MultisampleDesc msd{};
    msd.sampleNum = static_cast<nri::Sample_t>(desc.sampleNum);

    std::vector<nri::ColorAttachmentDesc> colors;
    for (auto f : desc.colorFormats) {
        nri::ColorAttachmentDesc ca{};
        ca.format = ToNriFormat(f);
        ca.colorWriteMask = nri::ColorWriteBits::RGBA;
        ca.blendEnabled = false;
        colors.push_back(ca);
    }

    nri::OutputMergerDesc omd{};
    omd.colors = colors.empty() ? nullptr : colors.data();
    omd.colorNum = static_cast<uint32_t>(colors.size());
    omd.depthStencilFormat = ToNriFormat(desc.depthStencilFormat);
    omd.depth.compareOp = desc.depthTest ? nri::CompareOp::ALWAYS : nri::CompareOp::ALWAYS;
    omd.depth.write = desc.depthWrite;
    omd.viewMask = desc.viewMask;
    if (desc.viewMask != 0) omd.multiview = nri::Multiview::LAYER_BASED;

    nri::GraphicsPipelineDesc gpd{};
    gpd.pipelineLayout = layout;
    gpd.vertexInput = (attrs.empty() && streams.empty()) ? nullptr : &vid;
    gpd.inputAssembly = iad;
    gpd.rasterization = rd;
    gpd.multisample = &msd;
    gpd.outputMerger = omd;
    gpd.shaders = shaders;
    gpd.shaderNum = 2;

    nri::Pipeline* pipeline = nullptr;
    if (nri_->core().CreateGraphicsPipeline(*nri_->device(), gpd, pipeline) !=
            nri::Result::SUCCESS ||
        pipeline == nullptr) {
        nri_->core().DestroyPipelineLayout(layout);
        error = "CreateGraphicsPipeline failed";
        return {};
    }

    auto* payload = new detail::PipelinePayload();
    payload->pipeline = pipeline;
    payload->layout = layout;
    payload->viewMask = desc.viewMask;

    ResourceId id = registry_.Create(payload, [this](void* p) {
        auto* pp = static_cast<detail::PipelinePayload*>(p);
        if (pp->pipeline) nri_->core().DestroyPipeline(pp->pipeline);
        if (pp->layout) nri_->core().DestroyPipelineLayout(pp->layout);
        delete pp;
    });
    pipelines_[id.index] = *payload;
    return id;
}

bool NriDevice::UploadBuffer(ResourceId buffer, const void* data, uint64_t sizeBytes,
                             uint64_t offset, std::string& error) {
    auto it = buffers_.find(buffer.index);
    if (it == buffers_.end()) {
        error = "UploadBuffer: unknown buffer";
        return false;
    }
    if (offset + sizeBytes > it->second.size) {
        error = "UploadBuffer: out of range";
        return false;
    }
    void* mapped = nri_->core().MapBuffer(*it->second.buffer, offset, sizeBytes);
    if (mapped == nullptr) {
        error = "MapBuffer failed";
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(sizeBytes));
    nri_->core().UnmapBuffer(*it->second.buffer);
    return true;
}

void NriDevice::Destroy(ResourceId id) { registry_.Destroy(id); }

// ---------------------------------------------------------------------------
// Command recording
// ---------------------------------------------------------------------------

bool NriDevice::BeginCommands(std::string& error) {
    if (recording_) {
        error = "BeginCommands: already recording";
        return false;
    }
    // The single command buffer may still be pending from a previous submit.
    // Resetting the allocator while it is in flight is a real lifetime bug
    // (VUID-vkResetCommandPool-commandPool-00040). Wait for the fence first.
    if (fence_ != nullptr) {
        const uint64_t completed = nri_->core().GetFenceValue(*fence_);
        if (completed < nextSubmitValue_ - 1) {
            nri_->core().Wait(*fence_, nextSubmitValue_ - 1);
            completedValue_ = nri_->core().GetFenceValue(*fence_);
            registry_.CompleteDomain(0, completedValue_);
        }
    }
    nri_->core().ResetCommandAllocator(*cmdAllocator_);
    if (nri_->core().BeginCommandBuffer(*cmdBuffer_, nullptr) != nri::Result::SUCCESS) {
        error = "BeginCommandBuffer failed";
        return false;
    }
    recordingRefs_.clear();
    recording_ = true;
    return true;
}

void NriDevice::BeginRenderPass(const semantic::RenderPassDesc& desc) {
    std::vector<nri::AttachmentDesc> colors;
    for (const auto& ca : desc.colorAttachments) {
        auto vit = views_.find(ca.view.index);
        if (vit == views_.end()) continue;
        nri::AttachmentDesc ad{};
        ad.descriptor = vit->second.descriptor;
        ad.loadOp = (ca.loadOp == semantic::LoadOp::kClear) ? nri::LoadOp::CLEAR
                                                            : nri::LoadOp::LOAD;
        ad.storeOp = (ca.storeOp == semantic::StoreOp::kStore) ? nri::StoreOp::STORE
                                                               : nri::StoreOp::DISCARD;
        ad.clearValue.color.f.x = ca.clearColor[0];
        ad.clearValue.color.f.y = ca.clearColor[1];
        ad.clearValue.color.f.z = ca.clearColor[2];
        ad.clearValue.color.f.w = ca.clearColor[3];
        colors.push_back(ad);
        RetainForCurrentRecording(ca.view);
    }

    nri::RenderingDesc rd{};
    rd.colors = colors.empty() ? nullptr : colors.data();
    rd.colorNum = static_cast<uint32_t>(colors.size());
    rd.viewMask = desc.viewMask;

    nri_->core().CmdBeginRendering(*cmdBuffer_, rd);
}

void NriDevice::EndRenderPass() { nri_->core().CmdEndRendering(*cmdBuffer_); }

void NriDevice::SetViewport(const semantic::Viewport& vp) {
    nri::Viewport v{};
    v.x = vp.x;
    v.y = vp.y;
    v.width = vp.width;
    v.height = vp.height;
    v.depthMin = vp.minDepth;
    v.depthMax = vp.maxDepth;
    nri_->core().CmdSetViewports(*cmdBuffer_, &v, 1);
}

void NriDevice::SetScissor(const semantic::Scissor& sc) {
    nri::Rect r{};
    r.x = static_cast<int16_t>(sc.x);
    r.y = static_cast<int16_t>(sc.y);
    r.width = static_cast<nri::Dim_t>(sc.width);
    r.height = static_cast<nri::Dim_t>(sc.height);
    nri_->core().CmdSetScissors(*cmdBuffer_, &r, 1);
}

void NriDevice::BindPipeline(ResourceId pipeline) {
    auto it = pipelines_.find(pipeline.index);
    if (it == pipelines_.end()) return;
    activePipeline_ = it->second.pipeline;
    activeLayout_ = it->second.layout;
    nri_->core().CmdSetPipelineLayout(*cmdBuffer_, nri::BindPoint::GRAPHICS, *activeLayout_);
    nri_->core().CmdSetPipeline(*cmdBuffer_, *activePipeline_);
    RetainForCurrentRecording(pipeline);
}

void NriDevice::SetVertexBuffer(uint32_t slot, ResourceId buffer, uint64_t offset,
                               uint32_t stride) {
    auto it = buffers_.find(buffer.index);
    if (it == buffers_.end()) return;
    nri::VertexBufferDesc vbd{};
    vbd.buffer = it->second.buffer;
    vbd.offset = offset;
    // When "features.extendedDynamicState" is supported NRI takes the stride
    // from here and ignores VertexStreamDesc::stride. Passing 0 would make
    // every vertex read from offset 0, so the caller's stride must be forwarded.
    vbd.stride = stride;
    nri_->core().CmdSetVertexBuffers(*cmdBuffer_, slot, &vbd, 1);
    RetainForCurrentRecording(buffer);
}

void NriDevice::SetIndexBuffer(ResourceId buffer, uint64_t offset,
                               semantic::IndexFormat format) {
    auto it = buffers_.find(buffer.index);
    if (it == buffers_.end()) return;
    nri_->core().CmdSetIndexBuffer(*cmdBuffer_, *it->second.buffer, offset,
                                   format == semantic::IndexFormat::kUint16
                                       ? nri::IndexType::UINT16
                                       : nri::IndexType::UINT32);
    RetainForCurrentRecording(buffer);
}

void NriDevice::SetShaderParameters(const semantic::ShaderParameterBlock& block) {
    if (block.data == nullptr || block.sizeBytes == 0) return;
    // Copy the bytes so the recorded command owns them. This is the snapshot
    // guarantee: later mutation of the caller's buffer cannot change the draw.
    paramScratch_.assign(static_cast<const unsigned char*>(block.data),
                         static_cast<const unsigned char*>(block.data) + block.sizeBytes);

    nri::SetRootConstantsDesc src{};
    src.rootConstantIndex = 0;
    src.data = paramScratch_.data();
    src.size = static_cast<uint32_t>(paramScratch_.size());
    src.offset = 0;
    src.bindPoint = nri::BindPoint::GRAPHICS;
    nri_->core().CmdSetRootConstants(*cmdBuffer_, src);
}

void NriDevice::TransitionResource(ResourceId id, semantic::ImageAspect aspect,
                                   semantic::ResourceState from,
                                   semantic::ResourceState to) {
    auto it = textures_.find(id.index);
    if (it == textures_.end()) return;

    nri::TextureBarrierDesc tb{};
    tb.texture = it->second.texture;
    tb.before.access = ToNriAccess(from);
    tb.before.layout = ToNriLayout(from);
    tb.before.stages = ToNriStages(from);
    tb.after.access = ToNriAccess(to);
    tb.after.layout = ToNriLayout(to);
    tb.after.stages = ToNriStages(to);
    tb.mipOffset = 0;
    tb.mipNum = 1;
    tb.layerOffset = 0;
    tb.layerNum = static_cast<nri::Dim_t>(it->second.layerNum);
    tb.planes = ToNriPlanes(aspect);

    nri::BarrierDesc bd{};
    bd.textures = &tb;
    bd.textureNum = 1;
    nri_->core().CmdBarrier(*cmdBuffer_, bd);
    RetainForCurrentRecording(id);
}

void NriDevice::DrawIndexed(const semantic::DrawIndexedDesc& desc) {
    nri::DrawIndexedDesc d{};
    d.indexNum = desc.indexCount;
    d.instanceNum = desc.instanceCount;
    d.baseIndex = desc.firstIndex;
    d.baseVertex = desc.baseVertex;
    d.baseInstance = desc.firstInstance;
    nri_->core().CmdDrawIndexed(*cmdBuffer_, d);
}

bool NriDevice::EndCommands(std::string& error) {
    if (!recording_) {
        error = "EndCommands: not recording";
        return false;
    }
    if (nri_->core().EndCommandBuffer(*cmdBuffer_) != nri::Result::SUCCESS) {
        error = "EndCommandBuffer failed";
        return false;
    }
    recording_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

bool NriDevice::Submit(uint64_t& outCompletionValue, std::string& error) {
    if (recording_) {
        error = "Submit: still recording";
        return false;
    }
    const uint64_t value = nextSubmitValue_++;

    // Retain every resource the recording touched, against this submission.
    for (const auto& id : recordingRefs_) {
        registry_.RetainForGpu(id, 0, value);
    }

    nri::FenceSubmitDesc signal{};
    signal.fence = fence_;
    signal.value = value;
    signal.stages = nri::StageBits::ALL;

    const nri::CommandBuffer* cmds[1] = {cmdBuffer_};
    nri::QueueSubmitDesc qsd{};
    qsd.commandBuffers = cmds;
    qsd.commandBufferNum = 1;
    qsd.signalFences = &signal;
    qsd.signalFenceNum = 1;

    if (nri_->core().QueueSubmit(*queue_, qsd) != nri::Result::SUCCESS) {
        // Submission failed: release the references we just took so the
        // resources are not stranded.
        for (const auto& id : recordingRefs_) {
            registry_.AbandonGpuRefs(id, 0, value);
        }
        error = "QueueSubmit failed";
        return false;
    }

    lastSubmitUsedNri_ = true;
    recordingRefs_.clear();
    outCompletionValue = value;
    return true;
}

bool NriDevice::WaitForCompletion(uint64_t value) {
    if (nri_ == nullptr || fence_ == nullptr) return false;
    nri_->core().Wait(*fence_, value);
    completedValue_ = nri_->core().GetFenceValue(*fence_);
    registry_.CompleteDomain(0, completedValue_);
    return true;
}

uint64_t NriDevice::CompletedValue() const {
    if (nri_ == nullptr || fence_ == nullptr) return 0;
    return nri_->core().GetFenceValue(*fence_);
}

// ---------------------------------------------------------------------------
// Verification (test-only, direct Vulkan)
// ---------------------------------------------------------------------------

bool NriDevice::ReadbackTexture(ResourceId id, uint32_t layer,
                                std::vector<unsigned char>& out, std::string& error) {
    auto it = textures_.find(id.index);
    if (it == textures_.end()) {
        error = "ReadbackTexture: unknown texture";
        return false;
    }
    const auto& tp = it->second;
    if (tp.vkImage == VK_NULL_HANDLE) {
        error = "ReadbackTexture: not an imported Vulkan image";
        return false;
    }

    // Direct Vulkan readback. This is verification only, never the render path.
    harness::Image img;
    img.image = tp.vkImage;
    img.memory = VK_NULL_HANDLE;
    img.ownsMemory = false;
    img.desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    img.desc.width = tp.width;
    img.desc.height = tp.height;
    img.desc.layerNum = tp.layerNum;

    VkCommandPool pool = vk_->CreateCommandPool(vk_->graphicsFamily());
    if (pool == VK_NULL_HANDLE) {
        error = "readback: command pool failed";
        return false;
    }
    const bool ok = ReadbackColor(*vk_, pool, img, layer, out, error);
    vkDestroyCommandPool(vk_->device(), pool, nullptr);
    return ok;
}

}  // namespace harness

namespace semantic {

std::unique_ptr<Device> CreateNriDevice(harness::VulkanContext& vk, std::string& error) {
    auto dev = std::make_unique<harness::NriDevice>();
    if (!dev->Initialize(vk, error)) return nullptr;
    return dev;
}

}  // namespace semantic

