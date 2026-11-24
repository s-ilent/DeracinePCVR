// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/amdgpu/regs_texture.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace VideoCore {

class Sampler {
public:
    explicit Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                     const AmdGpu::BorderColorBuffer border_color_base);
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    Sampler(Sampler&&) = default;
    Sampler& operator=(Sampler&&) = default;

    vk::Sampler Handle() const noexcept {
        return *handle;
    }

    /// The state the VkSampler was actually built from. Exposed so a draw-time observer records
    /// the effective state rather than re-deriving it and risking a different answer.
    bool AnisotropyEnabled() const noexcept {
        return anisotropy_enable;
    }
    float MaxAnisotropy() const noexcept {
        return max_anisotropy;
    }

private:
    vk::UniqueSampler handle;
    bool anisotropy_enable{};
    float max_anisotropy{1.0f};
};

} // namespace VideoCore
