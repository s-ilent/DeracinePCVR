#include "backend/nri_device.h"

#include <cstring>

#include "common/image_io.h"
#include "common/nri_adoption.h"

namespace harness {

namespace {

// How many independently recorded submissions may be outstanding at once.
constexpr size_t kCommandContextCount = 4;

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

VkFormat ToVkFormat(semantic::Format f) {
    switch (f) {
        case semantic::Format::kRGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case semantic::Format::kRGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case semantic::Format::kR32G32B32A32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case semantic::Format::kR32G32Float: return VK_FORMAT_R32G32_SFLOAT;
        case semantic::Format::kD32Float: return VK_FORMAT_D32_SFLOAT;
        case semantic::Format::kD24UnormS8: return VK_FORMAT_D24_UNORM_S8_UINT;
        case semantic::Format::kD32FloatS8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        default: return VK_FORMAT_UNDEFINED;
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

VkImageUsageFlags ToVkUsage(semantic::TextureUsage u) {
    VkImageUsageFlags f = 0;
    const auto raw = static_cast<uint32_t>(u);
    if (raw & static_cast<uint32_t>(semantic::TextureUsage::kColorAttachment))
        f |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (raw & static_cast<uint32_t>(semantic::TextureUsage::kDepthStencilAttachment))
        f |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (raw & static_cast<uint32_t>(semantic::TextureUsage::kSampled))
        f |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (raw & static_cast<uint32_t>(semantic::TextureUsage::kTransferSrc))
        f |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (raw & static_cast<uint32_t>(semantic::TextureUsage::kTransferDst))
        f |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return f;
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
            return static_cast<nri::AccessBits>(static_cast<uint32_t>(nri::AccessBits::COLOR_ATTACHMENT_WRITE));
        case semantic::ResourceState::kDepthStencilAttachment:
            return static_cast<nri::AccessBits>(static_cast<uint32_t>(nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE));
        case semantic::ResourceState::kShaderRead:
            return static_cast<nri::AccessBits>(static_cast<uint32_t>(nri::AccessBits::SHADER_RESOURCE));
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

    nri::FenceDesc fd{};
    fd.initialValue = 0;
    if (nri_->core().CreateFence(*nri_->device(), fd, fence_) != nri::Result::SUCCESS ||
        fence_ == nullptr) {
        error = "CreateFence failed";
        return false;
    }

    // Build the command-context pool. A partial failure must clean up.
    contexts_.resize(kCommandContextCount);
    for (size_t i = 0; i < contexts_.size(); ++i) {
        CommandContext& c = contexts_[i];
        if (nri_->core().CreateCommandAllocator(*queue_, c.allocator) != nri::Result::SUCCESS ||
            c.allocator == nullptr) {
            error = "CreateCommandAllocator failed";
            for (size_t j = 0; j < i; ++j) {
                if (contexts_[j].buffer) nri_->core().DestroyCommandBuffer(contexts_[j].buffer);
                if (contexts_[j].allocator) nri_->core().DestroyCommandAllocator(contexts_[j].allocator);
            }
            contexts_.clear();
            return false;
        }
        if (nri_->core().CreateCommandBuffer(*c.allocator, c.buffer) != nri::Result::SUCCESS ||
            c.buffer == nullptr) {
            error = "CreateCommandBuffer failed";
            for (size_t j = 0; j <= i; ++j) {
                if (contexts_[j].buffer) nri_->core().DestroyCommandBuffer(contexts_[j].buffer);
                if (contexts_[j].allocator) nri_->core().DestroyCommandAllocator(contexts_[j].allocator);
            }
            contexts_.clear();
            return false;
        }
        c.state = CommandContext::State::kAvailable;
    }
    return true;
}

void NriDevice::Shutdown() {
    if (nri_ && nri_->device() != nullptr) {
        // Drain every outstanding submission before destroying anything.
        nri_->core().DeviceWaitIdle(nri_->device());
        completedValue_ = nextSubmitValue_ - 1;
        registry_.CompleteDomain(0, completedValue_);
    }
    // The device is idle, so nothing can still reference a resource.
    registry_.DestroyAll();

    if (nri_ && nri_->device() != nullptr) {
        for (auto& c : contexts_) {
            if (c.buffer) nri_->core().DestroyCommandBuffer(c.buffer);
            if (c.allocator) nri_->core().DestroyCommandAllocator(c.allocator);
            c.buffer = nullptr;
            c.allocator = nullptr;
        }
        contexts_.clear();
        if (fence_) nri_->core().DestroyFence(fence_);
        fence_ = nullptr;
        nri_->Shutdown();
    }
    nri_.reset();
    active_ = nullptr;
    recording_ = false;
}

// ---------------------------------------------------------------------------
// Lookup and rejection
// ---------------------------------------------------------------------------

void NriDevice::Reject(const std::string& reason) {
    lastRejection_ = reason;
    // A rejected operation invalidates the recording: the command buffer no
    // longer describes what the caller asked for, so it must not be submitted
    // as if it succeeded.
    if (recording_) poisoned_ = true;
}

bool NriDevice::Acquire(ResourceId id, ResourceType type, const char* what) {
    LookupError err = LookupError::kOk;
    ResourceRecord* rec = registry_.Resolve(id, type, &err);
    if (rec == nullptr) {
        Reject(std::string(what) + ": " + LookupErrorName(err));
        return false;
    }
    if (rec->state != ResourceState::kLive) {
        Reject(std::string(what) + ": " + LookupErrorName(LookupError::kPendingDestroy));
        return false;
    }
    if (!recording_) {
        Reject(std::string(what) + ": no active recording");
        return false;
    }
    // Acquire the recording reference BEFORE the backend handle is used.
    if (!registry_.RetainForRecording(id, active_->recordingToken)) {
        Reject(std::string(what) + ": recording retention failed");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Command contexts
// ---------------------------------------------------------------------------

void NriDevice::ReapCompletedContexts() {
    if (nri_ == nullptr || fence_ == nullptr) return;
    const uint64_t done = nri_->core().GetFenceValue(*fence_);
    if (done > completedValue_) {
        completedValue_ = done;
        registry_.CompleteDomain(0, done);
    }
    for (auto& c : contexts_) {
        if (c.state == CommandContext::State::kSubmitted && c.submitValue <= done) {
            c.state = CommandContext::State::kCompleted;
        }
        if (c.state == CommandContext::State::kCompleted) {
            c.state = CommandContext::State::kAvailable;
            c.submitValue = 0;
        }
    }
}

CommandContext* NriDevice::AcquireContext() {
    ReapCompletedContexts();
    for (auto& c : contexts_) {
        if (c.state == CommandContext::State::kAvailable) return &c;
    }
    return nullptr;
}

bool NriDevice::WaitForContext(std::string& error) {
    // Backpressure: every context is in flight. Wait for the oldest.
    uint64_t oldest = UINT64_MAX;
    for (const auto& c : contexts_) {
        if (c.state == CommandContext::State::kSubmitted && c.submitValue < oldest) {
            oldest = c.submitValue;
        }
    }
    if (oldest == UINT64_MAX) {
        error = "no context available and none submitted";
        return false;
    }
    nri_->core().Wait(*fence_, oldest);
    ReapCompletedContexts();
    return true;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

ResourceId NriDevice::CreateBuffer(const semantic::BufferDesc& desc, std::string& error) {
    nri::BufferDesc bd{};
    bd.size = desc.size;
    bd.usage = ToNriBufferUsage(desc.usage);

    // Buffers the caller uploads into must be host-visible, because MapBuffer
    // only works on HOST_UPLOAD allocations.
    nri::Buffer* buf = nullptr;
    if (nri_->core().CreateCommittedBuffer(*nri_->device(), nri::MemoryLocation::HOST_UPLOAD,
                                           0.0f, bd, buf) != nri::Result::SUCCESS ||
        buf == nullptr) {
        error = "CreateCommittedBuffer failed";
        return {};
    }

    auto* payload = new detail::BufferPayload();
    payload->buffer = buf;
    payload->size = desc.size;

    return registry_.Create(ResourceType::kBuffer, payload, [this](void* p) {
        auto* bp = static_cast<detail::BufferPayload*>(p);
        if (bp->buffer) nri_->core().DestroyBuffer(bp->buffer);
        delete bp;
    });
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

    return registry_.Create(ResourceType::kTexture, payload, [this](void* p) {
        auto* tp = static_cast<detail::TexturePayload*>(p);
        if (tp->texture && !tp->external) nri_->core().DestroyTexture(tp->texture);
        delete tp;
    });
}

ResourceId NriDevice::ImportExternalImage(const semantic::ExternalImageDesc& desc,
                                          std::string& error) {
    if (desc.nativeHandle == nullptr) {
        error = "ImportExternalImage: null handle";
        return {};
    }
    const VkImage image = static_cast<VkImage>(desc.nativeHandle);
    const VkFormat vkFormat = ToVkFormat(desc.format);
    if (vkFormat == VK_FORMAT_UNDEFINED) {
        error = "ImportExternalImage: unknown format";
        return {};
    }

    nri::Texture* tex = nullptr;
    if (!nri_->WrapTexture(image, vkFormat, VK_IMAGE_TYPE_2D, ToVkUsage(desc.usage), desc.width,
                           desc.height, 1, 1, desc.layerNum, desc.sampleNum, tex, error)) {
        return {};
    }

    auto* payload = new detail::TexturePayload();
    payload->texture = tex;
    payload->external = true;   // the VkImage belongs to the caller
    payload->vkImage = image;
    payload->format = desc.format;
    payload->width = desc.width;
    payload->height = desc.height;
    payload->layerNum = desc.layerNum;
    payload->sampleNum = desc.sampleNum;

    return registry_.Create(ResourceType::kTexture, payload, [this](void* p) {
        auto* tp = static_cast<detail::TexturePayload*>(p);
        // Destroy the NRI wrapper only. The VkImage is not ours.
        if (tp->texture) nri_->core().DestroyTexture(tp->texture);
        delete tp;
    });
}

ResourceId NriDevice::CreateTextureView(const semantic::TextureViewDesc& desc,
                                        std::string& error) {
    LookupError err = LookupError::kOk;
    ResourceRecord* parentRec = registry_.Resolve(desc.texture, ResourceType::kTexture, &err);
    if (parentRec == nullptr) {
        error = std::string("CreateTextureView: parent texture ") + LookupErrorName(err);
        return {};
    }
    // A texture whose application ownership has been revoked must not acquire
    // new dependents. Its backend object may still be alive for in-flight work,
    // but new views would extend its life indefinitely.
    if (parentRec->state != ResourceState::kLive) {
        error = std::string("CreateTextureView: parent texture ") +
                LookupErrorName(LookupError::kPendingDestroy);
        return {};
    }
    auto* tp = static_cast<detail::TexturePayload*>(parentRec->backendObject);

    nri::TextureViewDesc vd{};
    vd.texture = tp->texture;
    // A view used as a render target must be declared as an attachment view.
    vd.type = (desc.aspect == semantic::ImageAspect::kColor)
                  ? nri::TextureView::COLOR_ATTACHMENT
                  : nri::TextureView::DEPTH_STENCIL_ATTACHMENT;
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

    const ResourceId viewId = registry_.Create(ResourceType::kTextureView, payload, [this](void* p) {
        auto* vp = static_cast<detail::ViewPayload*>(p);
        if (vp->descriptor) nri_->core().DestroyDescriptor(vp->descriptor);
        delete vp;
    });

    // The view's descriptor references the parent texture, so the parent must
    // outlive the view.
    if (!registry_.AddDependency(viewId, desc.texture)) {
        error = "CreateTextureView: dependency registration failed";
        registry_.Destroy(viewId);
        return {};
    }
    return viewId;
}

ResourceId NriDevice::CreateShader(const semantic::ShaderDesc& desc, std::string& error) {
    if (desc.bytecode.empty()) {
        error = "CreateShader: empty bytecode";
        return {};
    }
    auto* payload = new detail::ShaderPayload();
    payload->spirv = desc.bytecode;
    payload->stage = desc.stage;

    return registry_.Create(ResourceType::kShader, payload, [](void* p) {
        delete static_cast<detail::ShaderPayload*>(p);
    });
}

ResourceId NriDevice::CreateGraphicsPipeline(const semantic::GraphicsPipelineDesc& desc,
                                             std::string& error) {
    LookupError vsErr = LookupError::kOk, psErr = LookupError::kOk;
    detail::ShaderPayload* vs =
        registry_.Payload<detail::ShaderPayload>(desc.vertexShader, ResourceType::kShader, &vsErr);
    detail::ShaderPayload* ps =
        registry_.Payload<detail::ShaderPayload>(desc.pixelShader, ResourceType::kShader, &psErr);
    if (vs == nullptr || ps == nullptr) {
        error = std::string("CreateGraphicsPipeline: shader ") +
                LookupErrorName(vs == nullptr ? vsErr : psErr);
        return {};
    }

    nri::RootConstantDesc rc{};
    rc.registerIndex = 0;
    rc.size = 64;
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
    shaders[0].bytecode = vs->spirv.data();
    shaders[0].size = vs->spirv.size() * sizeof(uint32_t);
    shaders[0].entryPointName = "main";
    shaders[1].stage = nri::StageBits::FRAGMENT_SHADER;
    shaders[1].bytecode = ps->spirv.data();
    shaders[1].size = ps->spirv.size() * sizeof(uint32_t);
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
        nri::VertexStreamDesc vsd{};
        vsd.bindingSlot = static_cast<uint16_t>(b.slot);
        vsd.stepRate = nri::VertexStreamStepRate::PER_VERTEX;
        vsd.stride = static_cast<uint16_t>(b.stride);
        streams.push_back(vsd);
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
    omd.depth.compareOp = nri::CompareOp::ALWAYS;
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

    // NRI copies the shader bytecode during pipeline creation, so the pipeline
    // does not depend on the shader objects afterwards. No dependency edge.
    return registry_.Create(ResourceType::kPipeline, payload, [this](void* p) {
        auto* pp = static_cast<detail::PipelinePayload*>(p);
        if (pp->pipeline) nri_->core().DestroyPipeline(pp->pipeline);
        if (pp->layout) nri_->core().DestroyPipelineLayout(pp->layout);
        delete pp;
    });
}

bool NriDevice::UploadBuffer(ResourceId buffer, const void* data, uint64_t sizeBytes,
                             uint64_t offset, std::string& error) {
    LookupError err = LookupError::kOk;
    detail::BufferPayload* bp =
        registry_.Payload<detail::BufferPayload>(buffer, ResourceType::kBuffer, &err);
    if (bp == nullptr) {
        error = std::string("UploadBuffer: ") + LookupErrorName(err);
        return false;
    }
    if (offset + sizeBytes > bp->size) {
        error = "UploadBuffer: out of range";
        return false;
    }
    void* mapped = nri_->core().MapBuffer(*bp->buffer, offset, sizeBytes);
    if (mapped == nullptr) {
        error = "MapBuffer failed";
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(sizeBytes));
    nri_->core().UnmapBuffer(*bp->buffer);
    return true;
}

void NriDevice::Destroy(ResourceId id) {
    // The registry owns dependency release: it happens at physical destruction,
    // not here. Releasing it at Destroy would let a parent retire while a
    // GPU-retained view still referenced it.
    registry_.Destroy(id);
}

// ---------------------------------------------------------------------------
// Command recording
// ---------------------------------------------------------------------------

bool NriDevice::BeginCommands(std::string& error) {
    if (recording_) {
        error = "BeginCommands: already recording";
        return false;
    }
    // An executable context is still owned by the caller: it has been ended but
    // neither submitted nor abandoned. Overwriting active_ here would strand its
    // recording references and lose the command buffer.
    if (active_ != nullptr && active_->state == CommandContext::State::kExecutable) {
        error = "BeginCommands: an executable command buffer is awaiting Submit or "
                "AbandonRecording";
        return false;
    }
    CommandContext* ctx = AcquireContext();
    if (ctx == nullptr) {
        // Backpressure only: wait for the oldest outstanding submission.
        if (!WaitForContext(error)) return false;
        ctx = AcquireContext();
        if (ctx == nullptr) {
            error = "BeginCommands: no command context available";
            return false;
        }
    }

    // ResetCommandAllocator returns void. The context is only handed out when
    // its command buffer is known to have completed, so this reset is safe.
    nri_->core().ResetCommandAllocator(*ctx->allocator);
    if (nri_->core().BeginCommandBuffer(*ctx->buffer, nullptr) != nri::Result::SUCCESS) {
        error = "BeginCommandBuffer failed";
        return false;
    }

    ctx->state = CommandContext::State::kRecording;
    ctx->recordingToken = nextRecordingToken_++;   // independent of submit values
    active_ = ctx;
    recording_ = true;
    poisoned_ = false;
    lastRejection_.clear();
    paramScratch_.clear();
    return true;
}

void NriDevice::BeginRenderPass(const semantic::RenderPassDesc& desc) {
    if (!recording_ || active_ == nullptr) {
        Reject("BeginRenderPass: no active recording");
        return;
    }
    std::vector<nri::AttachmentDesc> colors;
    for (const auto& ca : desc.colorAttachments) {
        LookupError err = LookupError::kOk;
        detail::ViewPayload* vp =
            registry_.Payload<detail::ViewPayload>(ca.view, ResourceType::kTextureView, &err);
        if (vp == nullptr) {
            Reject(std::string("BeginRenderPass: view ") + LookupErrorName(err));
            return;
        }
        if (!Acquire(ca.view, ResourceType::kTextureView, "BeginRenderPass")) return;

        nri::AttachmentDesc ad{};
        ad.descriptor = vp->descriptor;
        ad.loadOp = (ca.loadOp == semantic::LoadOp::kClear) ? nri::LoadOp::CLEAR
                                                            : nri::LoadOp::LOAD;
        ad.storeOp = (ca.storeOp == semantic::StoreOp::kStore) ? nri::StoreOp::STORE
                                                               : nri::StoreOp::DISCARD;
        ad.clearValue.color.f.x = ca.clearColor[0];
        ad.clearValue.color.f.y = ca.clearColor[1];
        ad.clearValue.color.f.z = ca.clearColor[2];
        ad.clearValue.color.f.w = ca.clearColor[3];
        colors.push_back(ad);
    }

    nri::RenderingDesc rd{};
    rd.colors = colors.empty() ? nullptr : colors.data();
    rd.colorNum = static_cast<uint32_t>(colors.size());
    rd.viewMask = desc.viewMask;

    nri_->core().CmdBeginRendering(*active_->buffer, rd);
}

void NriDevice::EndRenderPass() {
    if (!recording_ || active_ == nullptr) {
        Reject("EndRenderPass: no active recording");
        return;
    }
    nri_->core().CmdEndRendering(*active_->buffer);
}

void NriDevice::SetViewport(const semantic::Viewport& vp) {
    if (!recording_ || active_ == nullptr) {
        Reject("SetViewport: no active recording");
        return;
    }
    nri::Viewport v{};
    v.x = vp.x;
    v.y = vp.y;
    v.width = vp.width;
    v.height = vp.height;
    v.depthMin = vp.minDepth;
    v.depthMax = vp.maxDepth;
    nri_->core().CmdSetViewports(*active_->buffer, &v, 1);
}

void NriDevice::SetScissor(const semantic::Scissor& sc) {
    if (!recording_ || active_ == nullptr) {
        Reject("SetScissor: no active recording");
        return;
    }
    nri::Rect r{};
    r.x = static_cast<int16_t>(sc.x);
    r.y = static_cast<int16_t>(sc.y);
    r.width = static_cast<nri::Dim_t>(sc.width);
    r.height = static_cast<nri::Dim_t>(sc.height);
    nri_->core().CmdSetScissors(*active_->buffer, &r, 1);
}

void NriDevice::BindPipeline(ResourceId pipeline) {
    LookupError err = LookupError::kOk;
    detail::PipelinePayload* pp =
        registry_.Payload<detail::PipelinePayload>(pipeline, ResourceType::kPipeline, &err);
    if (pp == nullptr) {
        Reject(std::string("BindPipeline: ") + LookupErrorName(err));
        return;
    }
    if (!Acquire(pipeline, ResourceType::kPipeline, "BindPipeline")) return;

    activePipeline_ = pp->pipeline;
    activeLayout_ = pp->layout;
    nri_->core().CmdSetPipelineLayout(*active_->buffer, nri::BindPoint::GRAPHICS, *activeLayout_);
    nri_->core().CmdSetPipeline(*active_->buffer, *activePipeline_);
}

void NriDevice::SetVertexBuffer(uint32_t slot, ResourceId buffer, uint64_t offset,
                               uint32_t stride) {
    LookupError err = LookupError::kOk;
    detail::BufferPayload* bp =
        registry_.Payload<detail::BufferPayload>(buffer, ResourceType::kBuffer, &err);
    if (bp == nullptr) {
        Reject(std::string("SetVertexBuffer: ") + LookupErrorName(err));
        return;
    }
    if (!Acquire(buffer, ResourceType::kBuffer, "SetVertexBuffer")) return;

    nri::VertexBufferDesc vbd{};
    vbd.buffer = bp->buffer;
    vbd.offset = offset;
    // When "features.extendedDynamicState" is supported NRI takes the stride
    // from here and ignores VertexStreamDesc::stride.
    vbd.stride = stride;
    nri_->core().CmdSetVertexBuffers(*active_->buffer, slot, &vbd, 1);
}

void NriDevice::SetIndexBuffer(ResourceId buffer, uint64_t offset,
                               semantic::IndexFormat format) {
    LookupError err = LookupError::kOk;
    detail::BufferPayload* bp =
        registry_.Payload<detail::BufferPayload>(buffer, ResourceType::kBuffer, &err);
    if (bp == nullptr) {
        Reject(std::string("SetIndexBuffer: ") + LookupErrorName(err));
        return;
    }
    if (!Acquire(buffer, ResourceType::kBuffer, "SetIndexBuffer")) return;

    nri_->core().CmdSetIndexBuffer(*active_->buffer, *bp->buffer, offset,
                                   format == semantic::IndexFormat::kUint16
                                       ? nri::IndexType::UINT16
                                       : nri::IndexType::UINT32);
}

void NriDevice::SetShaderParameters(const semantic::ShaderParameterBlock& block) {
    if (!recording_ || active_ == nullptr) {
        Reject("SetShaderParameters: no active recording");
        return;
    }
    if (block.data == nullptr || block.sizeBytes == 0) return;
    // Copy the bytes so the recorded command owns them. Later mutation of the
    // caller's buffer cannot change what the draw observes.
    paramScratch_.assign(static_cast<const unsigned char*>(block.data),
                         static_cast<const unsigned char*>(block.data) + block.sizeBytes);

    nri::SetRootConstantsDesc src{};
    src.rootConstantIndex = 0;
    src.data = paramScratch_.data();
    src.size = static_cast<uint32_t>(paramScratch_.size());
    src.offset = 0;
    src.bindPoint = nri::BindPoint::GRAPHICS;
    nri_->core().CmdSetRootConstants(*active_->buffer, src);
}

void NriDevice::TransitionResource(ResourceId id, semantic::ImageAspect aspect,
                                   semantic::ResourceState from,
                                   semantic::ResourceState to) {
    LookupError err = LookupError::kOk;
    detail::TexturePayload* tp =
        registry_.Payload<detail::TexturePayload>(id, ResourceType::kTexture, &err);
    if (tp == nullptr) {
        Reject(std::string("TransitionResource: ") + LookupErrorName(err));
        return;
    }
    if (!Acquire(id, ResourceType::kTexture, "TransitionResource")) return;

    nri::TextureBarrierDesc tb{};
    tb.texture = tp->texture;
    tb.before.access = ToNriAccess(from);
    tb.before.layout = ToNriLayout(from);
    tb.before.stages = ToNriStages(from);
    tb.after.access = ToNriAccess(to);
    tb.after.layout = ToNriLayout(to);
    tb.after.stages = ToNriStages(to);
    tb.mipOffset = 0;
    tb.mipNum = 1;
    tb.layerOffset = 0;
    tb.layerNum = static_cast<nri::Dim_t>(tp->layerNum);
    tb.planes = ToNriPlanes(aspect);

    nri::BarrierDesc bd{};
    bd.textures = &tb;
    bd.textureNum = 1;
    nri_->core().CmdBarrier(*active_->buffer, bd);
}

void NriDevice::DrawIndexed(const semantic::DrawIndexedDesc& desc) {
    if (!recording_ || active_ == nullptr) {
        Reject("DrawIndexed: no active recording");
        return;
    }
    nri::DrawIndexedDesc d{};
    d.indexNum = desc.indexCount;
    d.instanceNum = desc.instanceCount;
    d.baseIndex = desc.firstIndex;
    d.baseVertex = desc.baseVertex;
    d.baseInstance = desc.firstInstance;
    nri_->core().CmdDrawIndexed(*active_->buffer, d);
}

bool NriDevice::EndCommands(std::string& error) {
    if (!recording_ || active_ == nullptr) {
        error = "EndCommands: not recording";
        return false;
    }
    if (poisoned_) {
        error = "EndCommands: recording was invalidated by " + lastRejection_;
        AbandonRecording();
        return false;
    }
    if (nri_->core().EndCommandBuffer(*active_->buffer) != nri::Result::SUCCESS) {
        error = "EndCommandBuffer failed";
        AbandonRecording();
        return false;
    }
    active_->state = CommandContext::State::kExecutable;
    recording_ = false;
    return true;
}

void NriDevice::AbandonRecording() {
    if (active_ == nullptr) {
        recording_ = false;
        return;
    }
    // NRI requires EndCommandBuffer before the buffer can be begun again, but
    // only while it is actually recording. An executable context has already
    // been ended, and ending it twice is an error.
    if (active_->state == CommandContext::State::kRecording) {
        nri_->core().EndCommandBuffer(*active_->buffer);
    }
    // Release every reference the recording acquired.
    registry_.ReleaseRecording(active_->recordingToken);
    active_->state = CommandContext::State::kAvailable;
    active_->recordingToken = 0;
    active_ = nullptr;
    recording_ = false;
    poisoned_ = false;
}

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

bool NriDevice::Submit(uint64_t& outCompletionValue, std::string& error) {
    if (recording_) {
        error = "Submit: still recording";
        return false;
    }
    if (active_ == nullptr || active_->state != CommandContext::State::kExecutable) {
        error = "Submit: no executable command buffer";
        return false;
    }

    const uint64_t value = nextSubmitValue_++;

    nri::FenceSubmitDesc signal{};
    signal.fence = fence_;
    signal.value = value;
    signal.stages = nri::StageBits::ALL;

    const nri::CommandBuffer* cmds[1] = {active_->buffer};
    nri::QueueSubmitDesc qsd{};
    qsd.commandBuffers = cmds;
    qsd.commandBufferNum = 1;
    qsd.signalFences = &signal;
    qsd.signalFenceNum = 1;

    if (nri_->core().QueueSubmit(*queue_, qsd) != nri::Result::SUCCESS) {
        // Submission failed: release the recording references so nothing leaks
        // and no invalid work is resubmitted.
        registry_.ReleaseRecording(active_->recordingToken);
        active_->state = CommandContext::State::kAvailable;
        active_->recordingToken = 0;
        active_ = nullptr;
        error = "QueueSubmit failed";
        return false;
    }

    // Gap-free handoff: the recording references become GPU references.
    registry_.CommitRecording(active_->recordingToken, 0, value);
    active_->recordingToken = 0;
    active_->submitValue = value;
    active_->state = CommandContext::State::kSubmitted;
    active_ = nullptr;

    lastSubmitUsedNri_ = true;
    outCompletionValue = value;
    return true;
}

bool NriDevice::WaitForCompletion(uint64_t value) {
    if (nri_ == nullptr || fence_ == nullptr) return false;
    nri_->core().Wait(*fence_, value);
    ReapCompletedContexts();
    return true;
}

int NriDevice::OutstandingSubmissionCount() const {
    int n = 0;
    for (const auto& c : contexts_) {
        if (c.state == CommandContext::State::kSubmitted) ++n;
    }
    return n;
}

uint64_t NriDevice::CurrentRecordingToken() const {
    return active_ == nullptr ? 0 : active_->recordingToken;
}

bool NriDevice::HasExecutableContext() const {
    return active_ != nullptr && active_->state == CommandContext::State::kExecutable;
}

int NriDevice::AvailableContextCount() const {
    int n = 0;
    for (const auto& c : contexts_) {
        if (c.state == CommandContext::State::kAvailable) ++n;
    }
    return n;
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
    LookupError err = LookupError::kOk;
    detail::TexturePayload* tp =
        registry_.Payload<detail::TexturePayload>(id, ResourceType::kTexture, &err);
    if (tp == nullptr) {
        error = std::string("ReadbackTexture: ") + LookupErrorName(err);
        return false;
    }
    if (tp->vkImage == VK_NULL_HANDLE) {
        error = "ReadbackTexture: not an imported Vulkan image";
        return false;
    }

    harness::Image img;
    img.image = tp->vkImage;
    img.memory = VK_NULL_HANDLE;
    img.ownsMemory = false;
    img.desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    img.desc.width = tp->width;
    img.desc.height = tp->height;
    img.desc.layerNum = tp->layerNum;

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
